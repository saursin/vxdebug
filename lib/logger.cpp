#include "logger.h"
#include <map>

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
    if (g_file_.is_open())
        g_file_.close();
    g_file_.open(path, std::ios::app);
    if (!g_file_.is_open())
        std::cerr << "[Logger] Warning: failed to open log file: " << path << '\n';
}

void Logger::close_output_file() {
    if (g_file_.is_open())
        g_file_.close();
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

    std::string out;
    if (g_color_enabled_) {
        if(!prefix.empty())
            out += prefix_clr + "(" + prefix + ")" + ANSI_RST + " ";
        out += tag_clr_tab.at(lvl) + tag_tab.at(lvl) + ANSI_RST;
        out += msg_clr_tab.at(lvl) + msg + ANSI_RST + "\n";
    }
    else {
        if(!prefix.empty())
            out += "(" + prefix + ") ";
        out += tag_tab.at(lvl);
        out += msg + "\n";
    }
    
    // Thread-safe output
    std::lock_guard<std::mutex> lock(g_mutex_);
    
    // Always write to terminal
    std::cout << out;
    std::cout.flush();
    
    // Also write to file if open
    if (g_file_.is_open()) {
        std::string file_out;
        if(!prefix.empty())
            file_out += "(" + prefix + ") ";
        file_out += tag_tab.at(lvl);
        file_out += msg + "\n";
        
        g_file_ << file_out;
        g_file_.flush();
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
