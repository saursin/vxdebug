#pragma once
#include <string>
#include <vector>
#include <map>

// Forward declarations
class Backend;
class VortexDebugger;
class Logger;

typedef int (VortexDebugger::*CommandHandler_t)(const std::vector<std::string>&);
enum VxDbgState_t { STOPPED, RUNNING, EXIT };

class VortexDebugger {
public:
    VortexDebugger();
    ~VortexDebugger();

    int execute_command(const std::string &cmd, const std::vector<std::string>& args);
    int execute_script(const std::string &script);
    int start_cli();
    VxDbgState_t get_state() const { return running_; }

    // command handlers
    int cmd_help(const std::vector<std::string>& args);
    int cmd_exit(const std::vector<std::string>& args);
    int cmd_source(const std::vector<std::string>& args);
    int cmd_transport(const std::vector<std::string>& args);
    int cmd_reset(const std::vector<std::string>& args);
    int cmd_info(const std::vector<std::string>& args);
    int cmd_halt(const std::vector<std::string>& args);
    int cmd_continue(const std::vector<std::string>& args);
    int cmd_select(const std::vector<std::string>& args);
    int cmd_stepi(const std::vector<std::string>& args);
    int cmd_inject(const std::vector<std::string>& args);
    int cmd_reg(const std::vector<std::string>& args);
    int cmd_mem(const std::vector<std::string>& args);
    int cmd_dmreg(const std::vector<std::string>& args);
    int cmd_break(const std::vector<std::string>& args);

private:
    Logger *log_;
    Backend *backend_;
    VxDbgState_t running_ = STOPPED;

    // Helper function to register commands and aliases
    void register_command(const std::string& primary_name, 
                         const std::vector<std::string>& aliases,
                         const std::string& description,
                         CommandHandler_t handler);

    int __execute_line(const std::string &raw_input);
    
    // prompt generation
    std::string get_prompt() const;
    
    // utility functions
    void parse_warp_id_list(const std::string &wids_str, std::vector<int> &wids);
    
    // command table
    struct Command_t {
        std::string description;
        CommandHandler_t handler;
    };
    std::map<std::string, Command_t> command_;
    std::map<std::string, std::string> alias_map_;
};