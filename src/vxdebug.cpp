#include "vxdebug.h"
#include "logger.h"
#include <argparse.h>
#include "util.h"
#include "backend.h"

#include <sstream>
#include <algorithm>

#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

#define VXDBG_PROMPT "vxdbg> "

#define DEFAULT_TCP_IP   "127.0.0.1"
#define DEFAULT_TCP_PORT 5555

VortexDebugger::VortexDebugger():
    log_(new Logger("", 3)),
    backend_(new Backend())
{
    // Register commands using the helper function
    register_command("help",      {"h"},         "Show this help message", &VortexDebugger::cmd_help);
    register_command("exit",      {"quit", "q"}, "Exit the debugger", &VortexDebugger::cmd_exit);
    register_command("transport", {"t"},         "Set backend transport", &VortexDebugger::cmd_transport);
    register_command("source",    {"src"},       "Execute commands from a script file", &VortexDebugger::cmd_source);
    register_command("reset",     {"R"},         "Reset the target system", &VortexDebugger::cmd_reset);
    register_command("info",      {"i"},         "Display information about the target", &VortexDebugger::cmd_info);
    register_command("halt",      {"h"},         "Halt warps", &VortexDebugger::cmd_halt);
    register_command("continue",  {"c"},         "Continue/resume warps", &VortexDebugger::cmd_continue);
    register_command("select",    {"sel"},       "Select current warp and thread", &VortexDebugger::cmd_select);
    register_command("stepi",     {"s"},         "Single step instruction", &VortexDebugger::cmd_stepi);
    register_command("reg",       {"r"},         "Register operations", &VortexDebugger::cmd_reg);
    register_command("mem",       {"m"},         "Memory operations", &VortexDebugger::cmd_mem);
    register_command("dmreg",     {"d"},         "Debug module register operations", &VortexDebugger::cmd_dmreg);
    register_command("break",     {"b"},         "Breakpoint operations", &VortexDebugger::cmd_break);
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

int VortexDebugger::execute_script(const std::string &filepath) {
    log_->info("Executing script: " + filepath);
    std::string file_basename = basename(filepath);

    std::ifstream script_file(filepath);
    if (!script_file.is_open()) {
        log_->error("Failed to open script file: " + filepath);
        return 1;
    }

    running_ = RUNNING;

    std::string line;
    int line_num = 0;
    while (running_ == RUNNING && std::getline(script_file, line)) {
        line_num++;
        line = preprocess_commandline(line);
        if (line.empty()) continue; // skip blank lines
        printf(ANSI_YLW "%s:%d: %s\n" ANSI_RST, file_basename.c_str(), line_num, line.c_str());
        __execute_line(line);
    }

    script_file.close();
    if(running_ != EXIT) running_ = STOPPED;
    return 0;
}

int VortexDebugger::start_cli() {
    log_->info("Starting interactive CLI...");
    log_->info("Type 'help' for available commands, 'exit' to quit");

    running_ = RUNNING;
    std::string prev_input;

    while (running_ == RUNNING) {
        std::string input;
    #ifdef USE_READLINE
        // Read input using readline for better UX
        std::string prompt = get_prompt();
        char* raw_line = readline(prompt.c_str());
        if (!raw_line) {
            std::cout << std::endl;
            break; // EOF
        }
        input = std::string(raw_line);
        free(raw_line);
    #else
        // Simple getline if readline not available
        std::cout << get_prompt();
        if (!std::getline(std::cin, input)) {
            std::cout << std::endl;
            break; // EOF
        }
    #endif

        // preprocess: if input blank, use last command instead
        if(input == "") {
            input = prev_input;
        } else {
            prev_input = input;
        #ifdef USE_READLINE
            add_history(input.c_str());
        #endif
        }

        prev_input = input;

        input = preprocess_commandline(input);
        if (input.empty()) continue; // skip blank lines

        __execute_line(input);
    }

    if(running_ != EXIT) running_ = STOPPED;
    return 0;
}


int VortexDebugger::__execute_line(const std::string &input) {
    // --- Tokenize ---
    std::vector<std::string> toks = tokenize(input, ' ');
    if (toks.empty())
        return 0;
    
    // --- Execute ---
    std::string cmd = toks[0];
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
// Utility Functions
std::string VortexDebugger::get_prompt() const {
    std::string prompt = ANSI_GRN;
    
    // Connection indicator
    if (backend_->is_transport_connected()) {
        prompt += "● ";  // Connected indicator
    } else {
        prompt += "○ ";  // Disconnected indicator  
    }
    
    prompt += "vxdbg";
    
    // Add warp/thread selection info if available
    int selected_wid, selected_tid;
    backend_->get_selected_warp_thread(selected_wid, selected_tid);
    if (selected_wid >= 0 && selected_tid >= 0) {
        prompt += " [W" + std::to_string(selected_wid) + ":T" + std::to_string(selected_tid) + "]";
    }
    
    prompt += "> " + std::string(ANSI_RST);
    return prompt;
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
            out += strfmt("  %-20s - %s\n", cmd_display.c_str(), cmd_info.description.c_str());
        }
        
        log_->info("Available commands:\n" + out);
    }
    else if (args.size() == 2) {
        // Help for specific command
        const std::string& cmd = args[1];

        // If the command is not found, log an error
        if (execute_command(cmd, {cmd, "--help"}) == -1) {
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
    running_ = EXIT;
    return 0;
}

int VortexDebugger::cmd_source(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("source", "Execute commands from a script file");
    parser.add_argument({"script_file"}, "Path to script file", ArgParse::STR, "", true);
    int rc = parser.parse_args(args);
    if (rc != 0) {return rc;}

    const std::string& script_file = parser.get<std::string>("script_file");
    return execute_script(script_file);
}

int VortexDebugger::cmd_transport(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("transport", "Set backend transport");
    parser.add_argument({"--tcp"}, "Connect via TCP (host:port)", ArgParse::STR, "");
    int rc = parser.parse_args(args);
    if (rc != 0) {return rc;}

   if (parser.get<std::string>("tcp") != "") {
        log_->info("Setting transport to TCP");
        std::string transport_address = parser.get<std::string>("tcp");
        if (transport_address.empty()) {
            transport_address = std::string(DEFAULT_TCP_IP) + ":" + std::to_string(DEFAULT_TCP_PORT);
            log_->warn("No address specified, using default: " + transport_address);
        }

        std::string ip;
        uint16_t port;
        try {
            parse_tcp_hostportstr(transport_address, ip, port);
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
        log_->error("No transport type specified");
        return 1;
    }
   
    return 0;
}

int VortexDebugger::cmd_reset(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("reset", "Reset the target system");
    parser.add_argument({"-H", "--halt"}, "Halt all warps after reset", ArgParse::BOOL, "false");
    int rc = parser.parse_args(args);
    if (rc != 0) {return rc;}

    bool halt_warps = parser.get<bool>("halt");
    
    log_->info("Resetting target" + std::string(halt_warps ? " and halting warps" : ""));    
    backend_->reset(halt_warps);
    return 0;
}

int VortexDebugger::cmd_info(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("info", "Display information about the target");
    parser.add_argument({"info_type"}, "Type of information to display", ArgParse::STR, "", true, "", {"warps"});
    int rc = parser.parse_args(args);
    if (rc != 0) {return rc;}

    const std::string& info_type = parser.get<std::string>("info_type");

    if (info_type == "warps") {
        log_->info("Retrieving warp status...");
        auto warp_status = backend_->get_warp_status(true);
        std::string status = "Warp Status:\n";
        for (const auto& [wid, status_pair] : warp_status) {
            int coreid = wid / backend_->state_.platinfo.num_warps;
            bool halted = status_pair.first;
            uint32_t pc = status_pair.second;
            status += strfmt(" (Core: %2d) Warp %2d : (%-7s) PC=0x%08X\n", coreid, wid, halted ? "Halted" : "Running", halted ? pc : 0);
        }
        log_->info(status);
    }
    return 0;
}

int VortexDebugger::cmd_halt(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("halt", "Halt warps on the target");
    parser.add_argument({"-a", "--all"}, "Halt all warps", ArgParse::BOOL, "false");
    parser.add_argument({"-w", "--wids"}, "List of warp IDs to halt", ArgParse::STR, "", false, "", {}, "", "+");
    parser.add_argument({"-e", "--except"}, "Halt all warps except these IDs", ArgParse::STR, "", false, "", {}, "", "+");
    int rc = parser.parse_args(args);
    if (rc != 0) {return rc;}

    bool halt_all = parser.get<bool>("all");
    std::vector<std::string> wids_list = parser.get_list<std::string>("wids");
    std::vector<std::string> except_list = parser.get_list<std::string>("except");

    if (halt_all) {
        log_->info("Halting all warps...");
        backend_->halt_warps();
    } else if (!wids_list.empty()) {
        std::vector<int> wids;
        try {
            for (const auto& wid_str : wids_list) {
                int wid = std::stoi(wid_str);
                if (wid < 0 || static_cast<uint32_t>(wid) >= backend_->state_.platinfo.num_total_warps) {
                    throw std::runtime_error("Invalid warp ID: " + wid_str);
                }
                wids.push_back(wid);
            }
        } catch (const std::runtime_error& e) {
            log_->error("Error parsing warp IDs: " + std::string(e.what()));
            return 1;
        }

        log_->info("Halting specific warps: " + vecjoin<int>(wids));
        backend_->halt_warps(wids);
    }
    else if (!except_list.empty()) {
        // Halt all warps except the specified ones
        std::vector<int> except_wids;
        try {
            for (const auto& wid_str : except_list) {
                int wid = std::stoi(wid_str);
                if (wid < 0 || static_cast<uint32_t>(wid) >= backend_->state_.platinfo.num_total_warps) {
                    throw std::runtime_error("Invalid warp ID: " + wid_str);
                }
                except_wids.push_back(wid);
            }
        } catch (const std::runtime_error& e) {
            log_->error("Error parsing warp IDs: " + std::string(e.what()));
            return 1;
        }
        
        // Build list of all warps except the excluded ones
        std::vector<int> wids_to_halt;
        for (uint32_t wid = 0; wid < backend_->state_.platinfo.num_total_warps; wid++) {
            if (std::find(except_wids.begin(), except_wids.end(), static_cast<int>(wid)) == except_wids.end()) {
                wids_to_halt.push_back(static_cast<int>(wid));
            }
        }
        
        log_->info("Halting all warps except: " + vecjoin<int>(except_wids));
        backend_->halt_warps(wids_to_halt);
    } else {
        log_->error("Must specify either --all, --wids, or --except");
        return 1;
    }

    return 0;
}

int VortexDebugger::cmd_continue(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("continue", "Continue/resume warp execution");
    parser.add_argument({"-w", "--wids"}, "List of warp IDs to continue", ArgParse::STR, "", false, "", {}, "", "+");
    parser.add_argument({"-e", "--except"}, "Continue all warps except these IDs", ArgParse::STR, "", false, "", {}, "", "+");
    parser.add_argument({"-a", "--all"}, "Continue all warps", ArgParse::BOOL, "false");

    int rc = parser.parse_args(args);
    if (rc != 0) return rc;

    bool continue_all = parser.get<bool>("all");
    std::vector<std::string> wids_list = parser.get_list<std::string>("wids");
    std::vector<std::string> except_list = parser.get_list<std::string>("except");

    if (continue_all) {
        log_->info("Continuing all warps...");
        // For now, just use reset without halt - this will resume all warps
        backend_->reset(false);
    } 
    else if (!wids_list.empty()) {
        std::vector<int> wids;
        try {
            for (const auto& wid_str : wids_list) {
                int wid = std::stoi(wid_str);
                if (wid < 0 || static_cast<uint32_t>(wid) >= backend_->state_.platinfo.num_total_warps) {
                    throw std::runtime_error("Invalid warp ID: " + wid_str);
                }
                wids.push_back(wid);
            }
        } catch (const std::runtime_error& e) {
            log_->error("Error parsing warp IDs: " + std::string(e.what()));
            return 1;
        }
        log_->info("Continuing specific warps: " + vecjoin<int>(wids));
        backend_->resume_warps(wids);
    }
    else if (!except_list.empty()) {
        // Continue all warps except the specified ones
        std::vector<int> except_wids;
        try {
            for (const auto& wid_str : except_list) {
                int wid = std::stoi(wid_str);
                if (wid < 0 || static_cast<uint32_t>(wid) >= backend_->state_.platinfo.num_total_warps) {
                    throw std::runtime_error("Invalid warp ID: " + wid_str);
                }
                except_wids.push_back(wid);
            }
        } catch (const std::runtime_error& e) {
            log_->error("Error parsing warp IDs: " + std::string(e.what()));
            return 1;
        }
        
        // Build list of all warps except the excluded ones
        std::vector<int> wids_to_continue;
        for (uint32_t wid = 0; wid < backend_->state_.platinfo.num_total_warps; wid++) {
            if (std::find(except_wids.begin(), except_wids.end(), static_cast<int>(wid)) == except_wids.end()) {
                wids_to_continue.push_back(static_cast<int>(wid));
            }
        }
        
        log_->info("Continuing all warps except: " + vecjoin<int>(except_wids));
        backend_->resume_warps(wids_to_continue);
    } else {
        log_->error("Must specify either --all, --wids, or --except");
        return 1;
    }

    return 0;
}

int VortexDebugger::cmd_select(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("select", "Select current warp and thread for debugging");
    parser.add_argument({"wid"}, "Warp ID to select", ArgParse::INT, "0");
    parser.add_argument({"tid"}, "Thread ID to select (optional)", ArgParse::INT, "0");

    int rc = parser.parse_args(args);
    if (rc != 0) return rc;

    int wid = parser.get<int>("wid");
    int tid = parser.get<int>("tid");

    backend_->select_warp_thread(wid, tid);
    log_->info("Selected warp " + std::to_string(wid) + ", thread " + std::to_string(tid));

    return 0;
}

int VortexDebugger::cmd_stepi(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("stepi", "Single step instruction execution");
    
    int rc = parser.parse_args(args);
    if (rc != 0) return rc;

    log_->info("Single step not yet implemented");
    // TODO: Implement single step functionality
    // This requires step control via debug module registers
    
    return 0;
}

int VortexDebugger::cmd_reg(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("reg", "Register operations");
    parser.add_argument({"operation"}, "Operation: read or write", ArgParse::STR, "", true, "", {"read", "write"});
    parser.add_argument({"name"}, "Register name", ArgParse::STR, "");
    parser.add_argument({"value"}, "Value to write (for write operations)", ArgParse::STR, "");

    int rc = parser.parse_args(args);
    if (rc != 0) return rc;

    std::string operation = parser.get<std::string>("operation");
    std::string name = parser.get<std::string>("name");
    std::string value = parser.get<std::string>("value");

    if (operation == "read") {
        log_->info("Register read not yet implemented for: " + name);
        // TODO: Implement register read
    } else if (operation == "write") {
        if (value.empty()) {
            log_->error("Value required for write operation");
            return 1;
        }
        log_->info("Register write not yet implemented: " + name + " = " + value);
        // TODO: Implement register write
    } else {
        log_->error("Invalid operation. Use 'read' or 'write'");
        return 1;
    }

    return 0;
}

int VortexDebugger::cmd_mem(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("mem", "Memory operations");
    parser.add_argument({"operation"}, "Operation: read or write", ArgParse::STR, "", true, "", {"read", "write"});
    parser.add_argument({"address"}, "Memory address", ArgParse::STR, "");
    parser.add_argument({"data"}, "Data/length for read, or data bytes for write", ArgParse::STR, "");

    int rc = parser.parse_args(args);
    if (rc != 0) return rc;

    std::string operation = parser.get<std::string>("operation");
    std::string address = parser.get<std::string>("address");
    std::string data = parser.get<std::string>("data");

    if (operation == "read") {
        log_->info("Memory read not yet implemented: addr=" + address + ", len=" + data);
        // TODO: Implement memory read
    } else if (operation == "write") {
        log_->info("Memory write not yet implemented: addr=" + address + ", data=" + data);
        // TODO: Implement memory write
    } else {
        log_->error("Invalid operation. Use 'read' or 'write'");
        return 1;
    }

    return 0;
}

int VortexDebugger::cmd_dmreg(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("dmreg", "Debug module register operations");
    parser.add_argument({"operation"}, "Operation: read or write", ArgParse::STR, "", true, "", {"read", "write"});
    parser.add_argument({"name"}, "Register name", ArgParse::STR, "");
    parser.add_argument({"value"}, "Value to write (for write operations)", ArgParse::STR, "");

    int rc = parser.parse_args(args);
    if (rc != 0) return rc;

    std::string operation = parser.get<std::string>("operation");
    std::string name = parser.get<std::string>("name");
    std::string value = parser.get<std::string>("value");

    if (operation == "read") {
        log_->info("DM register read not yet implemented for: " + name);
        // TODO: Implement DM register read
    } else if (operation == "write") {
        if (value.empty()) {
            log_->error("Value required for write operation");
            return 1;
        }
        log_->info("DM register write not yet implemented: " + name + " = " + value);
        // TODO: Implement DM register write
    } else {
        log_->error("Invalid operation. Use 'read' or 'write'");
        return 1;
    }

    return 0;
}

int VortexDebugger::cmd_break(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("break", "Breakpoint operations");
    parser.add_argument({"operation"}, "Operation: set, del, or ls", ArgParse::STR, "", true, "", {"set", "del", "ls"});
    parser.add_argument({"address"}, "Breakpoint address", ArgParse::STR, "");

    int rc = parser.parse_args(args);
    if (rc != 0) return rc;

    std::string operation = parser.get<std::string>("operation");
    std::string address = parser.get<std::string>("address");

    if (operation == "set") {
        if (address.empty()) {
            log_->error("Address required for set operation");
            return 1;
        }
        log_->info("Breakpoint set not yet implemented at: " + address);
        // TODO: Implement breakpoint set
    } else if (operation == "del") {
        if (address.empty()) {
            log_->error("Address required for del operation");
            return 1;
        }
        log_->info("Breakpoint delete not yet implemented at: " + address);
        // TODO: Implement breakpoint delete
    } else if (operation == "ls") {
        log_->info("Breakpoint list not yet implemented");
        // TODO: Implement breakpoint list
    } else {
        log_->error("Invalid operation. Use 'set', 'del', or 'ls'");
        return 1;
    }

    return 0;
}
