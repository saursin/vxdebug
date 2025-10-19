#pragma once
#include <string>
#include <vector>
#include <cstdint>

// Return codes
#define RCODE_OK                 0
#define RCODE_ERROR             -1
#define RCODE_TIMEOUT           -2
#define RCODE_NOT_IMPL          -3
#define RCODE_INVALID_ARG       -4
#define RCODE_BUFFER_OVRFLW     -5
#define RCODE_COMM_ERR          -6
#define RCODE_TRANSPORT_ERR     -7
#define RCODE_NONESELECTED_ERR  -8

// ANSI colors
#define ANSI_RST "\033[0m"
#define ANSI_GRY "\033[90m"
#define ANSI_GRN "\033[32m"
#define ANSI_CYN "\033[36m"
#define ANSI_YLW "\033[33m"
#define ANSI_RED "\033[31m"


// String stripping functions
std::string lstrip(const std::string &s);
std::string rstrip(const std::string &s);
std::string strip(const std::string &s);

// Split string by delimiter (default: space)
std::vector<std::string> tokenize(const std::string &s, char delim=' ');

// Simple printf-style formatting function
std::string strfmt(const char* fmt, ...);

// Convert uint32_t -> string, optionally padded with prefix chars.
std::string hex2str(uint32_t value, size_t num_prefix = 0, char prefix_char = '0', bool uppercase = false);

// Parse a "host:port" string into IP and port components
void parse_tcp_hostportstr(const std::string &str, std::string &ip, uint16_t &port);

// Extract basename from a file path
std::string basename(const std::string &path);

// Preprocess command line: strip comments/whitespace
std::string preprocess_commandline(const std::string &input);

template<typename T>
std::string vecjoin(const std::vector<T> &vec, const std::string &sep = ",") {
    std::string result;
    for (size_t i = 0; i < vec.size(); ++i) {
        result += std::to_string(vec[i]);
        if (i != vec.size() - 1) {
            result += sep;
        }
    }
    return result;
}
