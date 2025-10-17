#pragma once
#include <iostream>
#include <fstream>
#include <string>
#include <mutex>

#define ANSI_RST "\033[0m"
#define ANSI_GRY "\033[90m"
#define ANSI_CYN "\033[36m"
#define ANSI_YLW "\033[33m"
#define ANSI_RED "\033[31m"

enum LogLevel {
    LOG_ERROR  = 0,
    LOG_WARN   = 1,
    LOG_INFO   = 2,
    LOG_DEBUG  = 3,
    LOG_DEBUG1 = 4,
    LOG_DEBUG2 = 5,
    LOG_DEBUG3 = 6,
    LOG_DEBUG4 = 7,
    LOG_DEBUG5 = 8,
    LOG_DEBUG6 = 9
};


class Logger {
public:
    Logger(const std::string &prefix = "", const int level = static_cast<int>(LOG_INFO), const int debug_thr = -1);
    ~Logger() = default;

    // === Global configuration ===
    static void set_global_prefix(const std::string& prefix) { g_prefix_ = prefix; }
    static void set_global_level(LogLevel level)             { g_level_ = level; }
    static void set_global_debug_threshold(int thr)          { g_debug_threshold_ = thr; }
    static void set_color_enabled(bool enable)               { g_color_enabled_ = enable; }
    static void set_output_file(const std::string& path);
    static void close_output_file();

    // === Per-instance logging ===
    void error(const std::string& msg) const;
    void warn (const std::string& msg) const;
    void info (const std::string& msg) const;
    void debug(const std::string& msg, int threshold = 3) const;

    // === Global logging (prefixed with 'g') ===
    static void gerror(const std::string& msg);
    static void gwarn (const std::string& msg);
    static void ginfo (const std::string& msg);
    static void gdebug(const std::string& msg, int threshold = 3);

private:
    // Shared global config
    static inline std::string g_prefix_ = "";
    static inline LogLevel g_level_ = LOG_INFO;
    static inline int g_debug_threshold_ = LOG_DEBUG;
    static inline bool g_color_enabled_ = true;
    
    static inline std::mutex g_mutex_;
    static inline std::ofstream g_file_;

    // Per-instance config
    std::string prefix_;
    LogLevel level_;
    int debug_threshold_;

    // Internal logging function
    static void log_internal(const Logger* self, LogLevel lvl,
                             const std::string& msg, int threshold = 3);
};
