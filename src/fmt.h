#pragma once
#include <string>
#include <string_view>

// On failure, returns the input and sets err. On success, clears err.
std::string formatSource(std::string_view src, std::string* err = nullptr);
int fmtCommand(int argc, char** argv);
