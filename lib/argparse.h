/**
 * @file argparse.h
 * @brief A lightweight and easy-to-use command-line argument parser for C++
 * @version 1.0.0
 * @author saursin
 * @date 2025
 * 
 * ArgparseC++ provides a simple interface for parsing command-line arguments 
 * with support for various data types, default values, and automatic help generation.
 */

#pragma once

#include <string>
#include <cstring>
#include <vector>
#include <map>
#include <variant>
#include <exception>
#include <stdexcept>

/// Maximum length for string arguments (legacy - no longer used with std::string)
#ifndef ARGPARSE_MAX_STRLEN
    #define ARGPARSE_MAX_STRLEN 512
#endif

namespace ArgParse {

/**
 * @brief Supported argument types
 */
enum ArgType_t {
    UNK,    // Unknown type
    BOOL,   // Boolean flag (true/false)
    INT,    // Integer number
    FLOAT,  // Floating-point number
    STR     // String value
};

/**
 * @brief Container for parsed argument values
 */
struct ArgVal_t {
    ArgType_t type;
    std::variant<bool, int, float, std::string> value;
};

/**
 * @brief Internal structure representing a command-line argument
 */
struct Argument_t {
    std::vector<std::string> aliases;   // Argument aliases (e.g., "-v", "--verbose")
    ArgType_t type      = UNK;          // Argument type
    std::string key     = "";           // Internal key name
    std::string help    = "";           // Help text
    bool required       = false;        // Whether argument is required
    ArgVal_t defaultval = {UNK, false};    // Default value (initialized to UNK type with false)
};

/**
 * @brief Convert alias to internal key name
 * @param alias Argument alias (e.g., "--opt-name")
 * @return Internal key name (e.g., "opt_name")
 * 
 * Converts command-line aliases to internal key names by:
 * - Removing leading dashes
 * - Converting remaining dashes to underscores
 * - Validating character set
 */
std::string alias2key(const std::string& alias);

/**
 * @brief Validate if string matches the specified type
 * @param str String to validate
 * @param type Expected argument type
 * @return true if valid, false otherwise
 * 
 * Validation rules:
 * - BOOL: "true", "1", "false", "0"
 * - INT: Integer numbers (including negative)
 * - FLOAT: Floating-point numbers (including negative)
 * - STR: Any string (always valid)
 */
bool is_valid_type(const std::string& str, ArgType_t type);

/**
 * @brief Main argument parser class
 * 
 * Provides a Python-argparse-like interface for parsing command-line arguments.
 * Supports multiple argument types, default values, required arguments, and
 * automatic help generation.
 * 
 * @example
 * ```cpp
 * ArgParse::ArgumentParser parser("myapp", "My application");
 * parser.add_argument({"-v", "--verbose"}, "Enable verbose output", ArgParse::BOOL);
 * parser.add_argument({"-f", "--file"}, "Input file", ArgParse::STR, "", true);
 * 
 * if (parser.parse_args(argc, argv) != 0) {
 *     return 1;
 * }
 * 
 * auto args = parser.get_opt_args();
 * if (std::get<bool>(args["verbose"].value)) {
 *     std::cout << "Verbose mode enabled\n";
 * }
 * ```
 */
class ArgumentParser {
private:
    std::string prog_name_;     ///< Program name
    std::string description_;   ///< Program description
    std::string epilog_;        ///< Additional help text
    
    std::vector<Argument_t>         arg_list_;          ///< List of defined arguments
    std::vector<std::string>        args_;              ///< Raw command-line arguments
    std::map<std::string, ArgVal_t> parsed_args_;       ///< Parsed optional arguments
    std::vector<std::string>        parsed_pos_args_;   ///< Parsed positional arguments

public:
    /**
     * @brief Construct a new Argument Parser
     * @param prog_name Program name (auto-detected from argv[0] if empty)
     * @param description Program description for help text
     * @param epilog Additional text displayed at end of help
     */
    ArgumentParser(const std::string& prog_name = "", 
                   const std::string& description = "", 
                   const std::string& epilog = "");
    
    /**
     * @brief Destructor
     */
    ~ArgumentParser() = default;

    /**
     * @brief Add a command-line argument
     * @param aliases List of argument names (e.g., {"-v", "--verbose"})
     * @param help Help text for this argument
     * @param type Argument type (BOOL, INT, FLOAT, STR)
     * @param defaultval Default value as string
     * @param required Whether this argument is required
     * @param key Custom internal key name (auto-generated if empty)
     * 
     * @throws ArgParseException if aliases is empty or invalid
     */
    void add_argument(const std::vector<std::string>& aliases, 
                      const std::string& help = "", 
                      ArgType_t type = BOOL, 
                      const std::string& defaultval = "", 
                      bool required = false, 
                      const std::string& key = "");

    /**
     * @brief Parse command-line arguments
     * @param argc Argument count from main()
     * @param argv Argument vector from main()
     * @return 0 on success, 1 if help was displayed, -1 on error
     * 
     * Parses the provided arguments according to the configured argument
     * definitions. Validates types, checks required arguments, and handles
     * help display automatically.
     */
    int parse_args(int argc, char** argv);

    /**
     * @brief Get parsed optional arguments
     * @return Map of argument keys to parsed values
     */
    const std::map<std::string, ArgVal_t>& get_opt_args() const { return parsed_args_; }

    /**
     * @brief Get parsed positional arguments
     * @return Vector of positional argument strings
     */
    const std::vector<std::string>& get_pos_args() const { return parsed_pos_args_; }

    /**
     * @brief Generic template method to get any argument type
     * @tparam T The type to retrieve (bool, int, float, std::string)
     * @param key Argument key name
     * @return Value of the specified type
     * @throws std::runtime_error if key not found or type mismatch
     * 
     * @example
     * ```cpp
     * bool verbose = parser.get<bool>("verbose");
     * std::string file = parser.get<std::string>("file");
     * int count = parser.get<int>("count");
     * float ratio = parser.get<float>("ratio");
     * ```
     */
    template<typename T>
    T get(const std::string& key) const {
        auto it = parsed_args_.find(key);
        if (it == parsed_args_.end()) {
            throw std::runtime_error("Argument key '" + key + "' not found. Make sure you defined it with add_argument().");
        }
        try {
            return std::get<T>(it->second.value);
        } catch (const std::bad_variant_access& e) {
            // Determine the actual stored type for better error message
            std::string actual_type;
            std::visit([&](auto&& arg) {
                using ArgType = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<ArgType, bool>) {
                    actual_type = "bool";
                } else if constexpr (std::is_same_v<ArgType, int>) {
                    actual_type = "int";
                } else if constexpr (std::is_same_v<ArgType, float>) {
                    actual_type = "float";
                } else if constexpr (std::is_same_v<ArgType, std::string>) {
                    actual_type = "std::string";
                } else {
                    actual_type = "unknown";
                }
            }, it->second.value);
            
            std::string requested_type;
            if constexpr (std::is_same_v<T, bool>) {
                requested_type = "bool";
            } else if constexpr (std::is_same_v<T, int>) {
                requested_type = "int";
            } else if constexpr (std::is_same_v<T, float>) {
                requested_type = "float";
            } else if constexpr (std::is_same_v<T, std::string>) {
                requested_type = "std::string";
            } else {
                requested_type = "unsupported type";
            }
            
            throw std::runtime_error("Type mismatch for argument '" + key + 
                                   "'. Expected: " + requested_type + 
                                   ", Got: " + actual_type);
        }
    }

    /**
     * @brief Check if an argument was explicitly provided by the user
     * @param key The argument key to check (uses underscore format: "no_cli")
     * @return true if argument was provided, false if using default value
     * 
     * Example:
     * ```cpp
     * if (parser.has_argument("verbose")) {
     *     std::cout << "User explicitly set verbose mode\n";
     * }
     * ```
     */
    bool has_argument(const std::string& key) const {
        return parsed_args_.find(key) != parsed_args_.end();
    }

    /**
     * @brief Get argument value with fallback default if not found or type mismatch
     * @tparam T The type to retrieve (bool, int, float, std::string)
     * @param key The argument key (uses underscore format: "no_cli")
     * @param default_value Value to return if key not found or type mismatch
     * @return The argument value or default_value
     * 
     * Example:
     * ```cpp
     * // Safe - won't throw, returns 8080 if port not set or wrong type
     * int port = parser.get_with_default<int>("port", 8080);
     * ```
     */
    template<typename T>
    T get_with_default(const std::string& key, const T& default_value) const {
        auto it = parsed_args_.find(key);
        if (it == parsed_args_.end()) {
            return default_value;
        }
        try {
            return std::get<T>(it->second.value);
        } catch (const std::bad_variant_access&) {
            return default_value;
        }
    }

    /**
     * @brief Get list of all available argument keys
     * @return Vector of all argument keys that were defined
     * 
     * Example:
     * ```cpp
     * for (const auto& key : parser.get_all_keys()) {
     *     std::cout << key << ": defined\n";
     * }
     * ```
     */
    std::vector<std::string> get_all_keys() const {
        std::vector<std::string> keys;
        for (const auto& pair : parsed_args_) {
            keys.push_back(pair.first);
        }
        return keys;
    }

    /**
     * @brief Print all parsed arguments (for debugging)
     */
    void print_args() const;

    /**
     * @brief Print help message
     */
    void print_help() const;
};


/**
 * @brief Exception thrown by argument parser on errors
 * 
 * Thrown when argument parsing encounters errors such as:
 * - Unknown arguments
 * - Missing required arguments  
 * - Invalid argument values
 * - Type validation failures
 */
class ArgParseException : public std::exception {
private:
    std::string message_;   ///< Error message

public:
    /**
     * @brief Construct exception with message
     * @param message Error description
     */
    explicit ArgParseException(const std::string& message) : message_(message) {}
    
    /**
     * @brief Get error message
     * @return C-style error string
     */
    const char* what() const noexcept override { return message_.c_str(); }
};

} // namespace ArgParse
