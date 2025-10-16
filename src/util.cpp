#include "util.h"

#include <string>
#include <vector>
#include <cstdarg>

std::string lstrip(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    return (start == std::string::npos) ? "" : s.substr(start);
}

std::string rstrip(const std::string &s) {
    size_t end = s.find_last_not_of(" \t\n\r");
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

std::string strip(const std::string &s) {
    return rstrip(lstrip(s));
}

std::vector<std::string> tokenize(const std::string &s, char delim) {
    std::vector<std::string> tokens;
    size_t start = 0;
    size_t end = s.find(delim);
    while (end != std::string::npos) {
        tokens.push_back(s.substr(start, end - start));
        start = end + 1;
        end = s.find(delim, start);
    }
    tokens.push_back(s.substr(start));
    return tokens;
}


// Simple printf-style formatting function
std::string strfmt(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    // Try to print into a small buffer first
    char small[128];
    int size = vsnprintf(small, sizeof(small), fmt, args);
    va_end(args);

    if (size < 0)
        return "<format error>";

    if (static_cast<size_t>(size) < sizeof(small)) {
        // Fits in small buffer
        return std::string(small, size);
    }

    // Fallback: Allocate exact size needed
    std::vector<char> buf(size + 1);
    va_start(args, fmt);
    vsnprintf(buf.data(), buf.size(), fmt, args);
    va_end(args);

    return std::string(buf.data(), size);
}