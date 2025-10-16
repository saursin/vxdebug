#include "util.h"

#include <string>
#include <vector>
#include <cstdarg>
#include <cstdint>
#include <stdexcept>

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

std::string hex2str(uint32_t value, size_t num_prefix, char prefix_char, bool uppercase) {
    std::string hex = strfmt(uppercase ? "%X" : "%x", value);
    if (num_prefix > hex.size()) {
        hex = std::string(num_prefix - hex.size(), prefix_char) + hex;
    }
    return hex;
}


// Parses a string of the form "IP:port" into its components.
// Either/Both IP and port can be omitted to use defaults.
void parse_tcp_hostportstr(const std::string &str, std::string &ip, uint16_t &port) {
    size_t colon_pos = str.find(':');
    if (colon_pos == std::string::npos) {
        throw std::runtime_error("Invalid TCP address format. Expected <IP>:<port>");
    }

    if(colon_pos > 0) {
        // IP part is present
        
        // Validate basic IP format (very basic check)
        size_t dot1 = str.find('.', 0);
        size_t dot2 = str.find('.', dot1 + 1);
        size_t dot3 = str.find('.', dot2 + 1);
        if (dot1 == std::string::npos || dot2 == std::string::npos || dot3 == std::string::npos || dot3 > colon_pos) {
            throw std::runtime_error("Invalid IP address format");
        }
        ip = str.substr(0, colon_pos);
    }
    else {
        ip = "";
    }

    if(colon_pos + 1 < str.size()) {
        // Port part is present
        int port_num = std::stoi(str.substr(colon_pos + 1));
        if (port_num <= 0 || port_num > 65535) {
            throw std::runtime_error("Invalid port number. Must be between 1 and 65535");
        }
        port = static_cast<uint16_t>(port_num);
    }
    else {
        port = 0; // Indicate no port specified
    }
}