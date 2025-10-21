#include "util.h"

#include <string>
#include <vector>
#include <cstdarg>
#include <cstdint>
#include <stdexcept>


std::string rcode_str(int code) {
    auto it = RCODE_STR_MAP.find(code);
    if (it != RCODE_STR_MAP.end()) {
        return it->second;
    } else {
        return "UNKNOWN_CODE";
    }
}


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
        ip = str.substr(0, colon_pos);
        if(ip == "localhost") {
            ip = "127.0.0.1";
        }
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

std::string basename(const std::string& path) {
    // Handle both Unix '/' and Windows '\\' separators
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos)
        return path; // no directory component
    return path.substr(pos + 1);
}

std::string preprocess_commandline(const std::string &input) {
    std::string line = input;
    size_t comment_pos = line.find('#');
    if (comment_pos != std::string::npos)
        line = line.substr(0, comment_pos);
    return strip(line);
}

uint32_t parse_uint(std::string str) {
    if (str.empty()) {
        throw std::runtime_error("Empty string cannot be parsed as uint");
    }
    if(str.size() > 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        // Hexadecimal
        return static_cast<uint32_t>(std::stoul(str, nullptr, 16));
    } else if (str.size() > 2 && str[0] == '0' && (str[1] == 'b' || str[1] == 'B')) {
        // Binary
        return static_cast<uint32_t>(std::stoul(str, nullptr, 2));
    } else {
        // Decimal
        return static_cast<uint32_t>(std::stoul(str, nullptr, 10));
    }
}

// Pretty hexdump for vector<uint8_t>
std::string hexdump(const std::vector<uint8_t>& data, size_t base_addr, size_t bytes_per_word, size_t words_per_col, bool enable_ascii) {
    if (bytes_per_word == 0 || words_per_col == 0) return "";

    std::string out;
    const size_t bytes_per_line = bytes_per_word * words_per_col;
    const size_t total_bytes = data.size();
    const size_t aligned_start = base_addr & ~(bytes_per_word - 1);

    for (size_t line_start = aligned_start; line_start < base_addr + total_bytes; line_start += bytes_per_line) {
        out += strfmt("%08X: ", static_cast<uint32_t>(line_start));

        // Print hex words
        for (size_t w = 0; w < words_per_col; ++w) {
            size_t word_addr = line_start + w * bytes_per_word;
            bool in_range = false;

            // Check if any byte in this word overlaps valid data range
            for (size_t b = 0; b < bytes_per_word; ++b) {
                if (word_addr + b >= base_addr && 
                    word_addr + b < base_addr + total_bytes) {
                    in_range = true;
                    break;
                }
            }

            if (!in_range) {
                for(size_t b = 0; b < bytes_per_word; ++b) {
                    out += "__";
                }
                out += " ";
                continue;
            }

            // Print each byte (MSB-first per word)
            for (int b = bytes_per_word - 1; b >= 0; --b) {
                size_t addr = word_addr + b;
                if (addr >= base_addr && addr < base_addr + total_bytes) {
                    uint8_t byte = data[addr - base_addr];
                    out += strfmt("%02X", byte);
                } else {
                    out += "__";
                }
            }
            out += ' ';
        }

        // ASCII section
        if (enable_ascii) {
            out += "| ";
            for (size_t b = 0; b < bytes_per_line; ++b) {
                size_t addr = line_start + b;
                if (addr >= base_addr && addr < base_addr + total_bytes) {
                    uint8_t ch = data[addr - base_addr];
                    out += (std::isprint(ch) ? static_cast<char>(ch) : '.');
                } else {
                    out += ' ';
                }
            }
        }

        out += "\n";
    }
    return out;
}
