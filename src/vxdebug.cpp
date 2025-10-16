#include "vxdebug.h"
#include "logger.h"
#include "util.h"
#include "backend.h"

#include <sstream>
#include <readline/readline.h>
#include <readline/history.h>

#define VXDBG_PROMPT "vxdbg> "

#define DEFAULT_TCP_IP   "127.0.0.1"
#define DEFAULT_TCP_PORT 5555

VortexDebugger::VortexDebugger(): 
    log_(new Logger("Vxdbg")),
    backend_(new Backend())
{
    // Register commands using the helper function
    register_command("help", {"h"}, "Show this help message", &VortexDebugger::cmd_help);
    register_command("exit", {"quit", "q"}, "Exit the debugger", &VortexDebugger::cmd_exit);
    register_command("transport", {"t"}, "Set backend transport", &VortexDebugger::cmd_transport);
}

VortexDebugger::~VortexDebugger() {
    if (backend_) {
        delete backend_;
    }
}

void VortexDebugger::register_command(const std::string& primary_name, 
                                     const std::vector<std::string>& aliases,
                                     const std::string& description,
                                     CommandHandler_t handler) {
    // Check if primary command already exists
    if (command_.find(primary_name) != command_.end()) {
        throw std::runtime_error("Command already registered: " + primary_name);
    }

    // Register the primary command
    command_[primary_name] = {description, handler};
    
    // Map primary command to itself in alias map
    alias_map_[primary_name] = primary_name;
    
    // Register all aliases pointing to the primary command
    for (const auto& alias : aliases) {
        alias_map_[alias] = primary_name;
    }
}

int VortexDebugger::execute_command(const std::string &cmd, const std::vector<std::string>& args) {
    // Look up the primary command name using alias map
    auto alias_it = alias_map_.find(cmd);
    if (alias_it == alias_map_.end()) {
        throw std::runtime_error("Unknown command: " + cmd);
    }
    
    // Get the primary command
    const std::string& primary_cmd = alias_it->second;
    auto cmd_it = command_.find(primary_cmd);
    if (cmd_it == command_.end()) {
        throw std::runtime_error("INTERNAL ERROR: Primary command not found: " + primary_cmd);
    }
    
    auto handler = cmd_it->second.handler;
    return (this->*handler)(args);
}

int VortexDebugger::execute_script(const std::string &path) {
    log_->info("Executing script: " + path);

    std::ifstream script_file(path);
    if (!script_file.is_open()) {
        log_->error("Failed to open script file: " + path);
        return 1;
    }

    running_ = true;

    std::string line;
    int line_num = 0;
    while (running_ && std::getline(script_file, line)) {
        line_num++;
        __execute_line(line, line_num, true);
    }

    script_file.close();
    running_ = false;
    return 0;
}

int VortexDebugger::start_cli() {
    log_->info("Starting interactive CLI...");
    log_->info("Type 'help' for available commands, 'exit' to quit");

    running_ = true;
    std::string prev_input;

    while (running_) {
        // Read input using readline for better UX
        char* raw_line = readline(ANSI_GRN VXDBG_PROMPT ANSI_RST);
        if (!raw_line) {
            std::cout << std::endl;
            break; // EOF
        }

        std::string input(raw_line);
        free(raw_line);

        // preprocess: if input blank, use last command instead
        if(input == "") {
            input = prev_input;
        } else {
            prev_input = input;
            add_history(input.c_str());
        }

        prev_input = input;

        __execute_line(input);
    }

    running_ = false;
    return 0;
}


int VortexDebugger::__execute_line(const std::string &raw_input, int line_num, bool show_echo) {
    std::string input = raw_input;

    // --- Preprocessing ---
    size_t comment_pos = input.find('#');
    if (comment_pos != std::string::npos)
        input = input.substr(0, comment_pos);
    input = strip(input);
    if (input.empty())
        return 0; // skip

    // --- Tokenize ---
    std::vector<std::string> toks = tokenize(input, ' ');
    if (toks.empty())
        return 0;

    std::string cmd = toks[0];

    if (show_echo) {
        printf(ANSI_GRN "%d: %s\n" ANSI_RST, line_num, input.c_str());
    }

    int result = 0;
    try {
        result = execute_command(cmd, toks);
    } catch (const std::exception &e) {
        log_->error(e.what());
        return 1;
    }

    if (result != 0 && result != -1) {
        log_->error("Command failed with code " + std::to_string(result));
    }
    return result;
}


////////////////////////////////////////////////////////////////////////////////
// Command Handlers
int VortexDebugger::cmd_help(const std::vector<std::string>& args) {
    if (args.size() == 1) {
        // Help for all commands - show primary commands with aliases
        std::string out;
        
        for (const auto& [primary_cmd, cmd_info] : command_) {
            // Find all aliases for this primary command
            std::vector<std::string> aliases;
            for (const auto& [alias, primary] : alias_map_) {
                if (primary == primary_cmd && alias != primary_cmd) {
                    aliases.push_back(alias);
                }
            }
            
            // Build display string
            std::string cmd_display = primary_cmd;
            if (!aliases.empty()) {
                cmd_display += " (";
                for (size_t i = 0; i < aliases.size(); ++i) {
                    if (i > 0) cmd_display += ", ";
                    cmd_display += aliases[i];
                }
                cmd_display += ")";
            }
            
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "  %-20s - %s\n", cmd_display.c_str(), cmd_info.description.c_str());
            out += buffer;
        }
        
        log_->info("Available commands:\n" + out);
    }
    else if (args.size() == 2) {
        // Help for specific command
        const std::string& cmd = args[1];

        // If the command is not found, log an error
        if (execute_command(cmd, {"--help"}) == -1) {
            log_->error("No help available for command: " + cmd);
        }
    } else {
        log_->error("Usage: help [command]");
        return 1;
    }
    return 0;
}

int VortexDebugger::cmd_exit(const std::vector<std::string>& args) {
    (void)args; // Suppress unused variable warning
    log_->info("Exiting...");
    running_ = false;
    return 0;
}

int VortexDebugger::cmd_transport(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        log_->error("Usage: transport <type> [options]\n Available transports: tcp");
        return 1;
    }
    
    const std::string& transport_type = args[1];
    
    if (transport_type == "tcp") {
        log_->info("Setting transport to TCP");
        std::string ip;
        uint16_t port;
        try {
            parse_tcp_hostportstr(args[2], ip, port);
        } catch (const std::runtime_error& e) {
            log_->error("Error parsing TCP address: " + std::string(e.what()));
            return 1;
        }

        if (ip.empty()) ip = DEFAULT_TCP_IP;
        if (port == 0)  port = DEFAULT_TCP_PORT;

        backend_->setup_transport("tcp");
        backend_->connect_transport({{"ip", ip}, {"port", std::to_string(port)}});

        backend_->initialize();

    } else {
        log_->error("Unknown transport type: " + transport_type);
        return 1;
    }
   
    return 0;
}

