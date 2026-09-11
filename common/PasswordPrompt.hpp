#pragma once

#include <string>

namespace forgefs::common {

// Prints `prompt` and reads a line from stdin with terminal echo disabled
// (POSIX termios), so a typed password doesn't appear on screen. Used by
// both the CLI client (`forgefs login`) and the coordinator's
// `--create-user` bootstrap mode.
std::string PromptPassword(const std::string& prompt);

}  // namespace forgefs::common
