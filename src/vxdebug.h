#pragma once
#include <string>
#include <vector>
#include <map>

// Forward declarations
class Backend;
class VortexDebugger;
class Logger;

typedef int (VortexDebugger::*CommandHandler_t)(const std::vector<std::string>&);

class VortexDebugger {
public:
    VortexDebugger();
    ~VortexDebugger();

    int execute_command(const std::string &cmd, const std::vector<std::string>& args);
    int execute_script(const std::string &script);
    int start_cli();

    // command handlers
    int cmd_help(const std::vector<std::string>& args);
    int cmd_exit(const std::vector<std::string>& args);
    int cmd_transport(const std::vector<std::string>& args);

private:
    Logger *log_;
    Backend *backend_;
    bool running_ = false;

    // Helper function to register commands and aliases
    void register_command(const std::string& primary_name, 
                         const std::vector<std::string>& aliases,
                         const std::string& description,
                         CommandHandler_t handler);

    int __execute_line(const std::string &raw_input, int line_num=0, bool show_echo=false);
    
    // command table
    struct Command_t {
        std::string description;
        CommandHandler_t handler;
    };
    std::map<std::string, Command_t> command_;
    std::map<std::string, std::string> alias_map_;
};