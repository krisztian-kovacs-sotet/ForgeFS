#include "common/PasswordPrompt.hpp"

#include <termios.h>
#include <unistd.h>

#include <iostream>

namespace forgefs::common {

std::string PromptPassword(const std::string& prompt) {
    std::cout << prompt << std::flush;

    termios old_termios{};
    tcgetattr(STDIN_FILENO, &old_termios);
    termios no_echo = old_termios;
    no_echo.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &no_echo);

    std::string password;
    std::getline(std::cin, password);

    tcsetattr(STDIN_FILENO, TCSANOW, &old_termios);
    std::cout << "\n";
    return password;
}

}  // namespace forgefs::common
