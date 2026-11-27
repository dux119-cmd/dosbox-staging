// SPDX-License-Identifier: GPL-2.0-or-later

// Command-line handling for rwzfs archives: a bare archive argument mounts it, and the flags --revise,
// --clear-changes and --create-archive run an archive action and exit.
//
// Two phases, because the command line is consumed early:
//   1. pre_parse(): before Config is built. It removes the archive flags from argv, and replaces a bare archive
//      argument with the archive's mount root, so Config copies the rewritten arguments. No logging here: the
//      logger is not running yet.
//   2. run(): after maybe_write_primary_config(), once the config directory is known (the overlay location is
//      under it). It runs the action, or mounts the archive, and logs what it does.

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace archive_cli {

// argv after pre_parse(). Keeps the strings alive: argv() points into them.
class Request {
public:
	Request(std::vector<std::string> args, std::optional<std::string> pre_error);

	int argc() const;
	const char* const* argv() const;

	// The archive named by a bare argument, if any (the path as given on the command line).
	const std::optional<std::string>& bare_archive() const;

	enum class Action { None, Revise, ClearChanges, CreateArchive, Extract, Mount };
	Action action() const;

	// The path given after the action flag (or the bare archive), and an error found while pre-parsing.
	const std::string& action_path() const;
	// The destination directory of --extract (empty for the other actions).
	const std::string& action_dest() const;
	const std::optional<std::string>& pre_error() const;

private:
	friend Request pre_parse(int argc, char* argv[]);
	std::vector<std::string> args_;
	std::vector<const char*> pointers_;
	std::optional<std::string> bare_archive_;
	std::optional<std::string> pre_error_;
	Action action_   = Action::None;
	std::string path_;
	std::string dest_;
};

// Phase 1: reads argv, strips the archive flags, and replaces a bare archive with its mount root.
Request pre_parse(int argc, char* argv[]);

// Phase 2: runs the requested action, or mounts the bare archive. Returns the exit code when the process should
// end now (an action ran, or it failed); nullopt when DOSBox should continue startup (the archive is mounted).
std::optional<int> run(const Request& request);

}  // namespace archive_cli
