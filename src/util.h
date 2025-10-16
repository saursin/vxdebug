#pragma once
#include <string>
#include <vector>

// Return codes
#define RCODE_OK                 0
#define RCODE_ERROR             -1
#define RCODE_TIMEOUT           -2
#define RCODE_NOT_IMPL          -3
#define RCODE_INVALID_ARG       -4
#define RCODE_BUFFER_OVRFLW     -5
#define RCODE_COMM_ERR          -6
#define RCODE_NOT_CONNECTED     -7

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