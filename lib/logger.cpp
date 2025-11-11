#include "logger.h"
#include <map>
#include <filesystem>

// Static member definitions
std::ofstream Logger::g_file_;
std::ofstream Logger::g_clean_file_;

const std::string prefix_clr = ANSI_GRY;

const std::map<LogLevel, std::string> tag_clr_tab = {
    {LOG_ERROR, ANSI_RED},
    {LOG_WARN,  ANSI_YLW},
    {LOG_INFO,  ANSI_CYN},
    {LOG_DEBUG, ANSI_GRY},
    {LOG_DEBUG1, ANSI_GRY},
    {LOG_DEBUG2, ANSI_GRY},
    {LOG_DEBUG3, ANSI_GRY},
    {LOG_DEBUG4, ANSI_GRY},
    {LOG_DEBUG5, ANSI_GRY},
    {LOG_DEBUG6, ANSI_GRY}
};

const std::map<LogLevel, std::string> tag_tab = {
    {LOG_ERROR, "[ERROR] "},
    {LOG_WARN,  "[!] "},
    {LOG_INFO,  "[+] "},
    {LOG_DEBUG, "[>] "},
    {LOG_DEBUG1, "[>] "},
    {LOG_DEBUG2, "[>] "},
    {LOG_DEBUG3, "[>] "},
    {LOG_DEBUG4, "[>] "},
    {LOG_DEBUG5, "[>] "},
    {LOG_DEBUG6, "[>] "}
};

const std::map<LogLevel, std::string> msg_clr_tab = {
    {LOG_ERROR, ""},
    {LOG_WARN,  ""},
    {LOG_INFO,  ""},
    {LOG_DEBUG, ANSI_GRY},
    {LOG_DEBUG1, ANSI_GRY},
    {LOG_DEBUG2, ANSI_GRY},
    {LOG_DEBUG3, ANSI_GRY},
    {LOG_DEBUG4, ANSI_GRY},
    {LOG_DEBUG5, ANSI_GRY},
    {LOG_DEBUG6, ANSI_GRY}
};


Logger::Logger(const std::string &prefix, const int debug_thr): 
    prefix_(prefix), 
    level_(LOG_INFO),  // Default level, will be overridden by global level
    debug_threshold_(debug_thr >= 0 ? debug_thr : g_debug_threshold_)
{}

void Logger::set_output_file(const std::string& path) {
    // Close existing files if open
    if (g_file_.is_open())
        g_file_.close();
    if (g_clean_file_.is_open())
        g_clean_file_.close();

    // Get path components
    std::filesystem::path log_path(path);
    auto parent_path = log_path.parent_path();
    auto stem = log_path.stem();
    auto ext = log_path.extension();
    
    // Create colored log file path
    auto colored_path = path;
    
    // Create clean log file path by inserting "_clean" before extension
    auto clean_path = (parent_path / (stem.string() + "_clean" + ext.string())).string();

    // Open both files
    g_file_.open(colored_path, std::ios::app);
    if (!g_file_.is_open())
        std::cerr << "[Logger] Warning: failed to open colored log file: " << colored_path << '\n';

    g_clean_file_.open(clean_path, std::ios::app);
    if (!g_clean_file_.is_open())
        std::cerr << "[Logger] Warning: failed to open clean log file: " << clean_path << '\n';
}

void Logger::close_output_file() {
    if (g_file_.is_open())
        g_file_.close();
    if (g_clean_file_.is_open())
        g_clean_file_.close();
}

// === Core logic ===
void Logger::log_internal(const Logger* self, LogLevel lvl,
                          const std::string& msg, int threshold) {
    const int level      = static_cast<int>(g_level_);  // Always use global level
    const int debug_thr  = self ? self->debug_threshold_ : g_debug_threshold_;
    const std::string& prefix = (self && !self->prefix_.empty()) ? self->prefix_ : g_prefix_;

    bool should_print = false;
    if (lvl >= LOG_DEBUG) {
        // If threshold is -1 (default), use the logger instance's debug threshold
        int effective_threshold = (threshold == -1) ? debug_thr : threshold;
        should_print = (level >= static_cast<int>(lvl) && level >= effective_threshold);
    }
    else {
        should_print = (level >= static_cast<int>(lvl));
    }

    if (!should_print)
        return;

    // Create colored output
    std::string colored_out;
    if (g_color_enabled_) {
        if(!prefix.empty())
            colored_out += prefix_clr + "(" + prefix + ")" + ANSI_RST + " ";
        colored_out += tag_clr_tab.at(lvl) + tag_tab.at(lvl) + ANSI_RST;
        colored_out += msg_clr_tab.at(lvl) + msg + ANSI_RST + "\n";
    }
    else {
        if(!prefix.empty())
            colored_out += "(" + prefix + ") ";
        colored_out += tag_tab.at(lvl);
        colored_out += msg + "\n";
    }

    // Create clean output (always without colors)
    std::string clean_out;
    if(!prefix.empty())
        clean_out += "(" + prefix + ") ";
    clean_out += tag_tab.at(lvl);
    clean_out += msg + "\n";
    
    // Thread-safe output
    std::lock_guard<std::mutex> lock(g_mutex_);
    if (g_file_.is_open()) {
        // Write colored output to main log file
        g_file_ << colored_out;
        g_file_.flush();

        // Write clean output to clean log file
        g_clean_file_ << clean_out;
        g_clean_file_.flush();

        // Write to terminal (colored)
        std::cout << colored_out;
        std::cout.flush();
    }
    else {
        // Just write to terminal if no files are open
        std::cout << colored_out;
        std::cout.flush();
    }
}

// === Per-instance methods ===
void Logger::error(const std::string& msg) const { log_internal(this, LOG_ERROR, msg); }
void Logger::warn (const std::string& msg) const { log_internal(this, LOG_WARN,  msg); }
void Logger::info (const std::string& msg) const { log_internal(this, LOG_INFO,  msg); }
void Logger::debug(const std::string& msg, int threshold) const { log_internal(this, LOG_DEBUG, msg, threshold); }

// === Global ('g' prefix) methods ===
void Logger::gerror(const std::string& msg) { log_internal(nullptr, LOG_ERROR, msg); }
void Logger::gwarn (const std::string& msg) { log_internal(nullptr, LOG_WARN,  msg); }
void Logger::ginfo (const std::string& msg) { log_internal(nullptr, LOG_INFO,  msg); }
void Logger::gdebug(const std::string& msg, int threshold) { log_internal(nullptr, LOG_DEBUG, msg, threshold); }
