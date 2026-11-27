// SPDX-License-Identifier: GPL-2.0-or-later

#include "archive/archive_cli.h"

#include "dosbox.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <ostream>
#include <streambuf>
#include <optional>
#include <string>
#include <vector>

#include "misc/cross.h"

// rwzfs: the session API and the archive operations, and the overlay lock's name.
#include "rwzfs/overlay/lock.h"
#include "rwzfs/rwz.h"

namespace archive_cli {

namespace {

namespace fs = std::filesystem;

constexpr const char* kFlagRevise        = "--revise";
constexpr const char* kFlagClearChanges  = "--clear-changes";
constexpr const char* kFlagCreateArchive = "--create-archive";
constexpr const char* kFlagExtract       = "--extract";

// The canonical extension is .dwarfs; .dfs is accepted as well. Case-insensitive.
bool is_archive_name(const std::string& arg)
{
	const auto dot = arg.rfind('.');
	if (dot == std::string::npos) {
		return false;
	}
	std::string ext = arg.substr(dot);
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return ext == ".dwarfs" || ext == ".dfs";
}

// Forwards each line written to it (dwarfs's own log) to LOG_INFO, so the archive writer's detail reaches the log.
class log_streambuf : public std::streambuf {
public:
	explicit log_streambuf(std::string tag) : tag_(std::move(tag)), line_() {}
	~log_streambuf() override { flush_line(); }

	log_streambuf(const log_streambuf&)            = delete;
	log_streambuf& operator=(const log_streambuf&) = delete;

protected:
	int_type overflow(int_type ch) override {
		if (ch == traits_type::eof()) {
			return traits_type::not_eof(ch);
		}
		if (ch == '\n') {
			flush_line();
		} else {
			line_.push_back(static_cast<char>(ch));
		}
		return ch;
	}

	int sync() override {
		flush_line();
		return 0;
	}

private:
	void flush_line() {
		if (!line_.empty()) {
			LOG_INFO("%s%s", tag_.c_str(), line_.c_str());
			line_.clear();
		}
	}

	std::string tag_;
	std::string line_;
};

// A size for progress and completion messages: 12.3 MiB.
std::string human_size(std::uint64_t bytes)
{
	const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
	double value        = static_cast<double>(bytes);
	std::size_t unit    = 0;
	while (value >= 1024.0 && unit + 1 < std::size(units)) {
		value /= 1024.0;
		++unit;
	}
	char text[32];
	std::snprintf(text, sizeof(text), unit == 0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
	return text;
}

// The archive's file name, for messages: the name as the user knows it, not the full path.
std::string file_name(const fs::path& p)
{
	return p.filename().string();
}

// Where overlays (saved changes) are kept: under the DOSBox configuration directory.
fs::path overlay_root()
{
	return get_config_dir() / "overlays";
}

rwz::session_options session_options()
{
	rwz::session_options options = {};
	options.overlay_root = overlay_root();
	return options;
}

// The lock file a mounted (or stale) archive leaves in its overlay directory, for messages.
std::string lock_file_for(const fs::path& archive)
{
	const auto overlay = rwz::overlay::overlay_directory(archive, overlay_root(), rwz::overlay::process_environment());
	if (!overlay) {
		return (overlay_root() / rwz::overlay::lock::kLockName).string();
	}
	return (*overlay / rwz::overlay::lock::kLockName).string();
}

// The default name for --create-archive: the directory's own name, with the canonical extension.
fs::path default_archive_name(const std::string& directory)
{
	auto dir = fs::absolute(fs::path(directory)).lexically_normal();
	if (dir.filename().empty()) {
		dir = dir.parent_path();  // a trailing separator: use the directory's own name
	}
	return dir.filename().string() + ".dwarfs";
}

// Messages name the archive by its file name, and say what went wrong with it.

std::string mount_error_text(rwz::MountError error, const fs::path& archive)
{
	const auto name = file_name(archive);
	switch (error) {
	case rwz::MountError::ArchiveNotFound:
		return name + " does not exist";
	case rwz::MountError::OpenFailed:
		return name + " is not a readable dwarfs archive, or it is damaged";
	case rwz::MountError::ArchiveInUse:
		return name + " is in use: another DOSBox instance has it mounted, or a stale lock file remains at " +
		       lock_file_for(archive) + " (delete it only if no DOSBox instance is running)";
	case rwz::MountError::WriteRootInvalid:
		return "the overlay location " + overlay_root().string() + " is not usable (needed for " + name + ")";
	case rwz::MountError::WriteRootUnavailable:
		return "the overlay location " + overlay_root().string() +
		       " could not be created or written (needed for " + name + "; check permissions and free space)";
	default:
		return name + " could not be mounted (error " + std::to_string(static_cast<int>(error)) + ")";
	}
}

std::string revise_error_text(rwz::revise_error error, const fs::path& archive)
{
	const auto name = file_name(archive);
	switch (error) {
	case rwz::revise_error::no_changes:
		return name + " has no changes to revise";
	case rwz::revise_error::io:
		return "the revision of " + name + " could not be written beside it (check free space and permissions)";
	default:
		return "the revision of " + name + " failed";
	}
}

std::string create_error_text(rwz::create_error error, const fs::path& archive, const std::string& directory)
{
	const auto name = file_name(archive);
	switch (error) {
	case rwz::create_error::source_missing:
		return "the directory " + directory + " does not exist (cannot create " + name + ")";
	case rwz::create_error::already_exists:
		return name + " already exists in the current folder (remove it, or run from another folder)";
	case rwz::create_error::unsupported_platform:
		return "creating archives is not supported on this platform";
	case rwz::create_error::tool_failed:
		return "the archive writer failed on " + directory + " while creating " + name + " (see the log)";
	case rwz::create_error::io:
		return name + " could not be written (check free space and permissions)";
	}
	return name + " could not be created";
}

std::string clear_error_text(rwz::clear_error error, const fs::path& archive)
{
	const auto name = file_name(archive);
	switch (error) {
	case rwz::clear_error::archive_in_use:
		return "the changes of " + name + " are in use: another DOSBox instance has it mounted, or a stale lock file "
		       "remains at " + lock_file_for(archive) + " (close that instance first; delete the lock file only if no "
		       "DOSBox instance is running)";
	case rwz::clear_error::io:
		return "the changes of " + name + " could not be cleared (check permissions)";
	}
	return "the changes of " + name + " could not be cleared";
}

std::string extract_error_text(rwz::extract_error error, const fs::path& archive, const std::string& dest)
{
	const auto name = file_name(archive);
	switch (error) {
	case rwz::extract_error::source_missing:
		return name + " does not exist or cannot be read";
	case rwz::extract_error::archive_in_use:
		return name + " is in use: another DOSBox instance has it mounted, or a stale lock file remains at " +
		       lock_file_for(archive) + " (close that instance first; delete the lock file only if no DOSBox instance "
		       "is running)";
	case rwz::extract_error::dest_unavailable:
		return "the destination " + dest + " is not a folder, or cannot be created (for " + name + ")";
	case rwz::extract_error::dest_not_empty:
		return "the destination " + dest + " is not empty; nothing was written for " + name +
		       " (choose a new or empty folder)";
	case rwz::extract_error::io:
		return "a file of " + name + " could not be copied to " + dest +
		       " (check free space and permissions; the folder holds a partial copy)";
	}
	return name + " could not be extracted";
}

// The mounted archive, kept until exit. Released by an atexit handler, which runs before DOSBox's own cleanup.
std::optional<rwz::session>& mounted_session()
{
	static std::optional<rwz::session> session;
	return session;
}

void unmount_at_exit()
{
	mounted_session().reset();
}

// "1m05s" style, for progress lines.
std::string elapsed_text(std::chrono::milliseconds elapsed)
{
	const auto total = static_cast<unsigned long long>(elapsed.count() / 1000);
	char text[32];
	std::snprintf(text, sizeof(text), "%llum%02llus", total / 60, total % 60);
	return text;
}

// The writer's progress, as one line about every two seconds. The writer reports completion itself.
void report_create_progress(const std::string& name, const rwz::create_progress& p)
{
	if (p.final) {
		return;
	}
	LOG_MSG("ARCHIVE: %s: %s written, %s elapsed", name.c_str(), human_size(p.bytes_written).c_str(),
	        elapsed_text(p.elapsed).c_str());
}

// Options for an archive write: the defaults, with dwarfs's log and the progress lines routed through LOG.
rwz::create_options write_options(const std::string& name, std::ostream& dwarfs_log)
{
	rwz::create_options options = {};
	options.log                 = &dwarfs_log;
	options.progress_interval   = std::chrono::seconds(2);
	options.on_progress         = [name](const rwz::create_progress& p) { report_create_progress(name, p); };
	return options;
}

int run_revise(const std::string& path_text)
{
	const fs::path archive = fs::absolute(path_text);
	auto opened = rwz::session::open(archive, session_options());
	if (!opened) {
		LOG_ERR("ARCHIVE: %s", mount_error_text(opened.error(), archive).c_str());
		return 1;
	}

	const auto changes = opened->changes();
	if (changes.empty()) {
		LOG_MSG("ARCHIVE: %s has no changes to revise", file_name(archive).c_str());
		return 0;
	}

	const auto name = file_name(archive);
	LOG_MSG("ARCHIVE: revising %s: %zu written, %zu deleted, %zu new folders", name.c_str(), changes.written,
	        changes.deleted, changes.new_dirs);
	log_streambuf dwarfs_buf("ARCHIVE: dwarfs: ");
	std::ostream dwarfs_log(&dwarfs_buf);
	const auto revised = opened->revise(write_options(name, dwarfs_log));
	if (!revised) {
		LOG_ERR("ARCHIVE: %s", revise_error_text(revised.error(), archive).c_str());
		return 1;
	}
	const auto shown = fs::path(path_text).parent_path() / revised->filename();
	std::error_code size_ec = {};
	const auto size = fs::file_size(*revised, size_ec);
	LOG_MSG("ARCHIVE: revised %s to %s (%s)", name.c_str(), shown.string().c_str(),
	        size_ec ? "size unknown" : human_size(size).c_str());
	return 0;
}

int run_clear_changes(const std::string& path_text)
{
	const fs::path archive = fs::absolute(path_text);
	const auto overlay = rwz::overlay::overlay_directory(archive, overlay_root(), rwz::overlay::process_environment());
	std::error_code ec = {};
	if (!overlay || !fs::exists(*overlay, ec)) {
		LOG_MSG("ARCHIVE: %s has no changes to clear", file_name(archive).c_str());
		return 0;
	}
	const auto cleared = rwz::clear_changes(archive, session_options());
	if (!cleared) {
		LOG_ERR("ARCHIVE: %s", clear_error_text(cleared.error(), archive).c_str());
		return 1;
	}
	LOG_MSG("ARCHIVE: cleared the changes of %s", file_name(archive).c_str());
	return 0;
}

int run_create_archive(const std::string& directory)
{
	const fs::path name = default_archive_name(directory);
	LOG_MSG("ARCHIVE: creating %s from %s", name.string().c_str(), directory.c_str());
	log_streambuf dwarfs_buf("ARCHIVE: dwarfs: ");
	std::ostream dwarfs_log(&dwarfs_buf);
	const auto created = rwz::archive_directory(name, fs::absolute(directory),
	                                             write_options(name.string(), dwarfs_log));
	if (!created) {
		LOG_ERR("ARCHIVE: %s", create_error_text(created.error(), name, directory).c_str());
		return 1;
	}
	std::error_code size_ec = {};
	const auto size = fs::file_size(*created, size_ec);
	LOG_MSG("ARCHIVE: created %s (%s)", name.string().c_str(),
	        size_ec ? "size unknown" : human_size(size).c_str());
	return 0;
}

int run_extract(const std::string& archive_text, const std::string& dest_text)
{
	const fs::path archive = fs::absolute(archive_text);
	const auto name        = file_name(archive);
	LOG_MSG("ARCHIVE: extracting %s to %s", name.c_str(), dest_text.c_str());
	const auto copied = rwz::extract_archive(
	    archive, fs::absolute(dest_text), session_options(), [name](const rwz::extract_stats& stats) {
		    LOG_MSG("ARCHIVE: extracting %s: %zu files, %s copied", name.c_str(), stats.files,
		            human_size(stats.bytes).c_str());
	    });
	if (!copied) {
		LOG_ERR("ARCHIVE: %s", extract_error_text(copied.error(), archive, dest_text).c_str());
		return 1;
	}
	LOG_MSG("ARCHIVE: extracted %s to %s: %zu files, %zu folders, %zu links (%s)", name.c_str(),
	        dest_text.c_str(), copied->files, copied->directories, copied->symlinks, human_size(copied->bytes).c_str());
	return 0;
}

// Mounts the bare archive. On success DOSBox continues, starting inside the mount root. Two log lines in all.
int run_mount(const std::string& path_text)
{
	const fs::path archive = fs::absolute(path_text);
	const auto name        = file_name(archive);

	auto opened = rwz::session::open(archive, session_options());
	if (!opened) {
		LOG_ERR("ARCHIVE: %s", mount_error_text(opened.error(), archive).c_str());
		return 1;
	}

	LOG_INFO("ARCHIVE: overlay for %s is kept in %s", name.c_str(), overlay_root().string().c_str());
	LOG_MSG("ARCHIVE: mounted %s at %s", path_text.c_str(), opened->root().string().c_str());

	static bool registered = false;
	if (!registered) {
		std::atexit(unmount_at_exit);
		registered = true;
	}
	// The working directory is inside the archive, so relative paths resolve there.
	std::error_code cwd_ec = {};
	rwz::fs::current_path(opened->root(), cwd_ec);
	if (cwd_ec) {
		LOG_ERR("ARCHIVE: could not enter %s: %s", opened->root().string().c_str(), cwd_ec.message().c_str());
		return 1;
	}
	mounted_session().emplace(std::move(*opened));
	return -1;  // continue startup
}

}  // namespace

Request::Request(std::vector<std::string> args, std::optional<std::string> pre_error)
        : args_(std::move(args)),
          pointers_(),
          bare_archive_(),
          pre_error_(std::move(pre_error)),
          action_(Action::None),
          path_(),
          dest_()
{
	for (const auto& a : args_) {
		pointers_.push_back(a.c_str());
	}
}

int Request::argc() const
{
	return static_cast<int>(pointers_.size());
}

const char* const* Request::argv() const
{
	return pointers_.data();
}

const std::optional<std::string>& Request::bare_archive() const
{
	return bare_archive_;
}

Request::Action Request::action() const
{
	return action_;
}

const std::string& Request::action_path() const
{
	return path_;
}

const std::string& Request::action_dest() const
{
	return dest_;
}

const std::optional<std::string>& Request::pre_error() const
{
	return pre_error_;
}

Request pre_parse(int argc, char* argv[])
{
	std::vector<std::string> kept;
	std::optional<std::string> error;
	std::optional<std::string> bare;
	Request::Action action = Request::Action::None;
	std::string action_path;
	std::string dest;

	if (argc > 0 && argv && argv[0]) {
		kept.emplace_back(argv[0]);  // the program name
	}

	auto set_action = [&](Request::Action a, const std::string& flag, int& i) {
		if (action != Request::Action::None) {
			error = "only one action can be given";
			return;
		}
		if (i + 1 >= argc) {
			error = "the " + flag + " option needs a path";
			return;
		}
		action      = a;
		action_path = argv[++i];
	};

	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == kFlagRevise) {
			set_action(Request::Action::Revise, arg, i);
		} else if (arg == kFlagClearChanges) {
			set_action(Request::Action::ClearChanges, arg, i);
		} else if (arg == kFlagCreateArchive) {
			set_action(Request::Action::CreateArchive, arg, i);
		} else if (arg == kFlagExtract) {
			if (action != Request::Action::None) {
				error = "only one action can be given";
			} else if (i + 2 >= argc) {
				error = "--extract needs an archive and a destination folder";
			} else {
				action      = Request::Action::Extract;
				action_path = argv[++i];
				dest        = argv[++i];
			}
		} else if (!arg.empty() && arg[0] != '-' && is_archive_name(arg)) {
			if (bare) {
				error = "only one archive can be given on the command line";
			} else {
				bare = arg;  // removed from the arguments: DOSBox never sees the archive, only the mounted root
			}
		} else {
			kept.push_back(arg);
		}
	}

	if (bare && action != Request::Action::None) {
		error = "an archive cannot be given with --revise, --clear-changes, --create-archive or --extract";
	}

	Request request(std::move(kept), std::move(error));
	request.bare_archive_ = bare;
	request.action_       = action;
	request.path_         = std::move(action_path);
	request.dest_         = std::move(dest);
	if (bare && !request.pre_error_) {
		request.action_ = Request::Action::Mount;
		request.path_   = *bare;
	}
	return request;
}

std::optional<int> run(const Request& request)
{
	if (request.pre_error()) {
		LOG_ERR("ARCHIVE: %s", request.pre_error()->c_str());
		return 2;
	}

	switch (request.action()) {
	case Request::Action::None:
		return std::nullopt;
	case Request::Action::Revise:
		return run_revise(request.action_path());
	case Request::Action::ClearChanges:
		return run_clear_changes(request.action_path());
	case Request::Action::CreateArchive:
		return run_create_archive(request.action_path());
	case Request::Action::Extract:
		return run_extract(request.action_path(), request.action_dest());
	case Request::Action::Mount: {
		const int rc = run_mount(request.action_path());
		return rc < 0 ? std::nullopt : std::optional<int>(rc);
	}
	}
	return std::nullopt;
}

}  // namespace archive_cli
