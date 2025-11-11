#include "vxdebug.h"
#include "logger.h"
#include <argparse.h>
#include "util.h"
#include "dmdefs.h"
#include "backend.h"
#include "gdbstub.h"

#include <sstream>
#include <algorithm>
#include <unistd.h>
#include <cstdlib>

#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
    #ifndef HISTORY_FILE
        #define HISTORY_FILE ".vxdbg_history"
    #endif
    #ifndef MAX_HISTORY_ENTRIES
        #define MAX_HISTORY_ENTRIES 1000
    #endif
#endif

#define VXDBG_PROMPT "vxdbg> "

#define DEFAULT_TCP_IP   "127.0.0.1"
#define DEFAULT_TCP_PORT 5555

#define CHECK_ERRS(stmt) \
    do { \
        int rc = stmt; \
        if (rc != RCODE_OK) { \
            return rc; \
        } \
    } while(0)


VortexDebugger::VortexDebugger():
    log_(new Logger("", 3)),
    backend_(new Backend())
{
    // Register commands using the helper function
    register_command("help",      {"h"},         "Show this help message", &VortexDebugger::cmd_help);
    register_command("exit",      {"quit", "q"}, "Exit the debugger", &VortexDebugger::cmd_exit);
    register_command("init",      {},            "Initialize the target program", &VortexDebugger::cmd_init);
    register_command("transport", {"t"},         "Set backend transport", &VortexDebugger::cmd_transport);
    register_command("source",    {"src"},       "Execute commands from a script file", &VortexDebugger::cmd_source);
    register_command("reset",     {"R"},         "Reset the target system", &VortexDebugger::cmd_reset);
    register_command("info",      {"i"},         "Display information about the target", &VortexDebugger::cmd_info);
    register_command("halt",      {"h"},         "Halt warps", &VortexDebugger::cmd_halt);
    register_command("continue",  {"c"},         "Continue/resume warps", &VortexDebugger::cmd_continue);
    register_command("select",    {"sel"},       "Select current warp and thread", &VortexDebugger::cmd_select);
    register_command("stepi",     {"s"},         "Single step instruction", &VortexDebugger::cmd_stepi);
    register_command("inject",    {"inj"},       "Inject instruction", &VortexDebugger::cmd_inject);
    register_command("reg",       {"r"},         "Register operations", &VortexDebugger::cmd_reg);
    register_command("mem",       {"m"},         "Memory operations", &VortexDebugger::cmd_mem);
    register_command("dmreg",     {"d"},         "Debug module register operations", &VortexDebugger::cmd_dmreg);
    register_command("break",     {"b"},         "Breakpoint operations", &VortexDebugger::cmd_break);
    register_command("gdbserver", {"gdb"},       "Start GDB server", &VortexDebugger::cmd_gdbserver);
    register_command("param",     {},            "Get/Set debugger parameters", &VortexDebugger::cmd_param);

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
        log_->info("Script " + file_basename + ":" + std::to_string(line_num) + ": " + line);
        int rc = __execute_line(line);
        if (rc != 0) {
            log_->error(strfmt("Script execution halted due to error at %s:%d", file_basename.c_str(), line_num));
            break;
        }
    }

    script_file.close();
    if(running_ != EXIT) running_ = STOPPED;
    return 0;
}

int VortexDebugger::start_cli() {
    log_->info("Starting interactive CLI...");
    log_->info("Type 'help' for available commands, 'exit' to quit");

#ifdef USE_READLINE
    // Initialize readline history
    using_history();
    
    // Try to read existing history file
    std::string history_file = HISTORY_FILE;
    read_history(history_file.c_str());
#endif

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
            if (!input.empty()) {
                HIST_ENTRY* last = history_get(history_length);
                if (!last || input != last->line) {
                    add_history(input.c_str());
                }
            }
        #endif
        }

        input = preprocess_commandline(input);
        if (input.empty()) continue; // skip blank lines

        __execute_line(input);
    }

#ifdef USE_READLINE
    // Save history before exiting
    write_history(history_file.c_str());
    
    // Limit history file size to last 1000 entries
    history_truncate_file(history_file.c_str(), MAX_HISTORY_ENTRIES);
#endif

    if(running_ != EXIT) running_ = STOPPED;
    return 0;
}


int VortexDebugger::__execute_line(const std::string &input) {
    // --- Tokenize ---
    std::vector<std::string> toks = tokenize(input, ' ');
    if (toks.empty())
        return 0;
    
    // Log the command being executed
    log_->info("Command: " + input);
    
    // --- Execute ---
    std::string cmd = toks[0];
    int result = 1;
    try {
        result = execute_command(cmd, toks);
    } catch (const std::exception &e) {
        log_->error("Caught Exception: " + std::string(e.what()));
    }

    if (result != RCODE_OK) {
        log_->error(strfmt("Command failed with code: %d, %s", result, rcode_str(result).c_str()));
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////
// Utility Functions
std::string VortexDebugger::get_prompt() const {
    std::string prompt = ANSI_GRN;
    
    // Connection indicator
    if (backend_->transport_connected()) {
        prompt += "● ";  // Connected indicator
    } else {
        prompt += "○ ";  // Disconnected indicator  
    }
    
    prompt += "vxdbg";
    
    if(backend_->transport_connected() ) {
        // Add warp/thread selection info if available
        int selected_wid, selected_tid;
        backend_->get_selected_warp_thread(selected_wid, selected_tid, true);

        uint32_t selected_pc;
        backend_->get_warp_pc(selected_pc);
        
        if (selected_wid >= 0 && selected_tid >= 0) {
            prompt += strfmt(" [W%d:T%d, PC=0x%08X]", selected_wid, selected_tid, selected_pc);
        }
    }
    
    prompt += "> " + std::string(ANSI_RST);
    return prompt;
}

////////////////////////////////////////////////////////////////////////////////
// Command Handlers
int VortexDebugger::cmd_help(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("help", "Show help for commands");
    parser.add_argument({"command"}, "Command to show help for", ArgParse::STR, "");
    int rc = parser.parse_args(args);
    if (rc != 0) {return rc;}

    const std::string& command = parser.get<std::string>("command");

    if (!command.empty()) {
        // Help for specific command
        execute_command(command, {command, "--help"});
    }
    else {
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
    return RCODE_OK;
}

int VortexDebugger::cmd_exit(const std::vector<std::string>& args) {
    (void)args; // Suppress unused variable warning
    log_->info("Exiting...");
    running_ = EXIT;
    return 0;
}

int VortexDebugger::cmd_init(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("init", "Initialize the target program");
    // parser.add_argument({"-r", "--reset"}, "Reset target before running", ArgParse::BOOL, "false");
    int rc = parser.parse_args(args);
    if (rc != 0) {return rc;}

    // bool reset = parser.get<bool>("reset");

    log_->info("Initializing target platform...");
    rc = backend_->initialize();
    if (rc != RCODE_OK) {
        log_->error("Failed to start target execution");
        return rc;
    }
    // running_ = RUNNING;   // TODO: do we need this?
    return RCODE_OK;
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

        int rc;
        rc = backend_->transport_setup("tcp");
        if (rc != RCODE_OK) return rc;
        rc = backend_->transport_connect({{"ip", ip}, {"port", std::to_string(port)}});
        if (rc != RCODE_OK) return rc;
    } else {
        log_->error("No transport type specified, see 'help transport' for usage.");
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
    backend_->reset_platform(halt_warps);
    return 0;
}

int VortexDebugger::cmd_info(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("info", "Display information about the target");
    parser.add_argument({"info_type"}, "Type of information to display", ArgParse::STR, "warps", false, "", {"w", "warps", "p", "platform"});
    parser.add_argument({"-w", "--wid"}, "List of warp IDs", ArgParse::STR, "", false, "", {}, "", "+");
    parser.add_argument({"-l", "--long"}, "Display long format", ArgParse::BOOL, "false");
    int rc = parser.parse_args(args);
    if (rc != 0) {return rc;}

    const std::string& info_type = parser.get<std::string>("info_type");
    const std::vector<std::string>& wids = parser.get_list<std::string>("wid");
    bool long_format = parser.get<bool>("long");

    if (info_type == "warps" || info_type == "w") {
        log_->info("Retrieving warp status...");
        std::map<int, WarpStatus_t> warp_status;
        backend_->get_warp_status(warp_status, true, true);
        std::string status = "";
        unsigned n_total = 0;
        uint32_t n_active = 0;
        uint32_t n_halted = 0;

        for (const auto& [wid, status_tuple] : warp_status) {
            if (!wids.empty() && std::find(wids.begin(), wids.end(), std::to_string(wid)) == wids.end()) {
                continue; // skip if not in specified list
            }
            n_total++;
            int coreid = wid / backend_->state_.platinfo.num_warps;
            bool active = status_tuple.active;
            if (active) n_active++;
            bool halted = status_tuple.halted;
            if (halted) n_halted++;
            uint32_t pc = status_tuple.pc;
            uint32_t hacause = status_tuple.hacause;
            std::string hacause_str = hacause_tostr(hacause);

            std::string active_status_clr, active_status_str;
            std::string haltrun_status_clr, haltrun_status_str;
            bool show_pc = false;
            bool show_cause = false;

            if (active) {
                active_status_clr = ANSI_GRN;
                active_status_str = "Active";
                haltrun_status_clr = halted ? ANSI_RED : ANSI_GRN;
                haltrun_status_str = halted ? "Halted" : "Running";
                show_pc = halted ? true : false;
                show_cause = halted ? true : false;
            } 
            else {
                active_status_clr = ANSI_YLW;
                active_status_str = "Inactive";
                haltrun_status_clr = ANSI_GRY;
                haltrun_status_str = halted ? "Halted" : "Unhalted";
                show_pc = true;
                show_cause = true;
            }
            
            if(long_format) {
                status += strfmt("  (Core:%d) Warp %2d: %s%-8s" ANSI_RST " %s%-8s" ANSI_RST "  PC=", coreid, wid, 
                    active_status_clr.c_str(), active_status_str.c_str(),
                    haltrun_status_clr.c_str(), haltrun_status_str.c_str());
                    status += ANSI_BLU + (show_pc ? strfmt("0x%08X ", pc) : "0x________ ") + ANSI_RST;
                status += show_cause ? strfmt("(Cause %x: %s)", hacause, hacause_str.c_str()) : "";
                status += "\n";
            }
            else {
                uint32_t warps_per_row = backend_->state_.platinfo.num_warps;
                status += strfmt("%3d:%s%s" ANSI_RST ",%s%s" ANSI_RST ":", wid,
                    active_status_clr.c_str(), active_status_str.substr(0, 1).c_str(),
                    haltrun_status_clr.c_str(), haltrun_status_str.substr(0, 1).c_str());

                status += ANSI_BLU + (show_pc ? strfmt("0x%08X", pc) : "0x________") + ANSI_RST;
                status += show_cause ? strfmt(",%s", hacause_str.substr(0, 1).c_str()) : "  ";
                status += "  ";
                if ((wid + 1) % warps_per_row == 0) {
                    status += "\n";
                }
            }
        }
        log_->info("Warp Status: \n" + strfmt("Showing status for %d warps: (Halted: %d warps)\n", n_total, n_halted) + status);
    }
    else if (info_type == "platform" || info_type == "p") {
        auto platinfo_str = backend_->_get_platform_info_str();
        log_->info("Platform Information:\n" + platinfo_str);
    }
    else {
        log_->error("Unknown info type: " + info_type + ", see 'help info' for usage.");
        return 1;
    }
    return 0;
}

int VortexDebugger::cmd_halt(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("halt", "Halt warps on the target");
    parser.add_argument({"-a", "--all"}, "Halt all warps", ArgParse::BOOL, "false");
    parser.add_argument({"-w", "--wid"}, "List of warp IDs to halt", ArgParse::STR, "", false, "", {}, "", "+");
    parser.add_argument({"-e", "--except"}, "Halt all warps except these IDs", ArgParse::STR, "", false, "", {}, "", "+");
    int rc = parser.parse_args(args);
    if (rc != 0) {return rc;}

    bool halt_all = parser.get<bool>("all");
    std::vector<std::string> wids_list = parser.get_list<std::string>("wid");
    std::vector<std::string> except_list = parser.get_list<std::string>("except");

    if (halt_all) {                     // Halt all warps
        log_->info("Halting all warps...");
        backend_->halt_warps();
    } 
    else if (!wids_list.empty()) {      // Halt specific warps
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
    else if (!except_list.empty()) {    // Halt all warps except the specified ones
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
    } 
    else {                              // Halt currently selected warp
        int selected_wid, selected_tid;
        backend_->get_selected_warp_thread(selected_wid, selected_tid, true);
        if (selected_wid < 0) {
            log_->error("No warp selected to halt. Use --all or --wid to specify warps.");
            return 1;
        }
        log_->info("Halting currently selected warp: " + std::to_string(selected_wid));
        backend_->halt_warps({selected_wid});
    }

    return 0;
}

int VortexDebugger::cmd_continue(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("continue", "Continue/resume warp execution");
    parser.add_argument({"-w", "--wid"}, "List of warp IDs to continue", ArgParse::STR, "", false, "", {}, "", "+");
    parser.add_argument({"-e", "--except"}, "Continue all warps except these IDs", ArgParse::STR, "", false, "", {}, "", "+");
    parser.add_argument({"-a", "--all"}, "Continue all warps", ArgParse::BOOL, "false");

    int rc = parser.parse_args(args);
    if (rc != 0) return rc;

    bool continue_all = parser.get<bool>("all");
    std::vector<std::string> wids_list = parser.get_list<std::string>("wid");
    std::vector<std::string> except_list = parser.get_list<std::string>("except");

    if (continue_all) {                 // Continue all warps
        log_->info("Continuing all warps...");
        backend_->resume_warps();

        bool any_breakpoints;
        CHECK_ERRS(backend_->any_breakpoints(any_breakpoints));
        if (any_breakpoints) {
            log_->info("Breakpoints set, Continuing until breakpoint...");
            backend_->until_breakpoint(true);
        }
    } 
    else if (!wids_list.empty()) {      // Continue specific warps
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
    else if (!except_list.empty()) {    // Continue all warps except the specified ones
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
    } 
    else {                              // Continue currently selected warp
        int selected_wid, selected_tid;
        backend_->get_selected_warp_thread(selected_wid, selected_tid, true);
        if (selected_wid < 0) {
            log_->error("No warp selected to continue. Use --all or --wid to specify warps.");
            return 1;
        }
        log_->info("Continuing currently selected warp: " + std::to_string(selected_wid));
        backend_->resume_warps({selected_wid});
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
    parser.add_argument({"count"}, "Number of instructions to step", ArgParse::INT, "1");
        
    int rc = parser.parse_args(args);
    if (rc != 0) return rc;

    int count = parser.get<int>("count");
    
    for (int i = 0; i < count; i++) {
        log_->info("Executing single step " + std::to_string(i + 1) + "/" + std::to_string(count));
        
        rc = backend_->step_warp();
        if (rc != 0) {
            log_->error("Single step failed");
            return 1;
        }
    }
    return 0;
}

int VortexDebugger::cmd_inject(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("inject", "Inject instruction into selected warp/thread");
    parser.add_argument({"instruction"}, "32-bit instruction value (hex or decimal)", ArgParse::STR, "", true);
        
    int rc = parser.parse_args(args);
    if (rc != 0) return rc;

    // check if a warp is selected
    int selected_wid, selected_tid;
    backend_->get_selected_warp_thread(selected_wid, selected_tid, true);
    if (selected_wid < 0 || selected_tid < 0) {
        log_->error("No warp/thread selected for instruction injection");
        return 1;
    }

    // check if current warp is halted
    bool is_active;
    bool is_halted;
    CHECK_ERRS(backend_->get_warp_state(backend_->state_.selected_wid, is_active, is_halted));
    if (!is_active) {
        log_->error("Cannot inject instruction: selected warp is not active");
        return RCODE_WARP_NOT_ACTIVE;
    }
    if (!is_halted) {
        log_->error("Cannot inject instruction: selected warp is not halted");
        return RCODE_WARP_NOT_HALTED;
    }

    // Get instruction to inject
    std::string instr = parser.get<std::string>("instruction");
    try {
        uint32_t instr_word = parse_uint(instr);
        CHECK_ERRS(backend_->inject_instruction(instr_word));
    } catch (const std::exception& e) {
        // If parsing as uint failed, treat as assembly instruction
        std::string asm_instr = instr;
        CHECK_ERRS(backend_->inject_instruction(asm_instr));
    }
    
    log_->info("Injected instruction into warp " + std::to_string(selected_wid) + ", thread " + std::to_string(selected_tid));
    return 0;
}

int VortexDebugger::cmd_reg(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("reg", "Register operations");
    parser.add_argument({"operation"}, "Operation: read(r), write(w)", ArgParse::STR, "", true, "", {"r", "w", "read", "write"});
    parser.add_argument({"name"}, "Register name", ArgParse::STR, "");
    parser.add_argument({"value"}, "Value to write (for write operations)", ArgParse::STR, "");
    int rc = parser.parse_args(args);
    if (rc != 0) return rc;

    std::string operation = parser.get<std::string>("operation");
    std::string name = parser.get<std::string>("name");

    if (operation == "r" || operation == "read") {
        uint32_t reg_value;
        CHECK_ERRS(backend_->read_reg(name, reg_value));
        log_->info(strfmt("Register %s = 0x%08X (%u)", name.c_str(), reg_value, reg_value));
    } 
    else if (operation == "w" || operation == "write") {
        uint32_t reg_value = parse_uint(parser.get<std::string>("value"));
        CHECK_ERRS(backend_->write_reg(name, reg_value));
        log_->info(strfmt("Register %s written with 0x%08X (%u)", name.c_str(), reg_value, reg_value));
    }
    else {
        log_->error("Invalid operation. See 'help reg' for usage.");
        return 1;
    }

    return 0;
}

int VortexDebugger::cmd_mem(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("mem", "Memory operations");
    parser.add_argument({"operation"}, "Operation: read(r) or write(w)", ArgParse::STR, "", true, "", {"r", "w", "read", "write", "loadbin"});
    parser.add_argument({"address"}, "Memory address", ArgParse::STR, "");
    parser.add_argument({"length"}, "Length in bytes (for read)", ArgParse::INT, "4");
    parser.add_argument({"value"}, "Comma-separated list of values to write (for write operations)", ArgParse::STR, "");
    parser.add_argument({"-a", "--ascii"}, "Display memory as ASCII (for read operations)", ArgParse::BOOL, "false");
    parser.add_argument({"-b", "--bytes"}, "Display memory as bytes (for read operations)", ArgParse::BOOL, "false");
    int rc = parser.parse_args(args);
    if (rc != 0) return rc;

    std::string operation = parser.get<std::string>("operation");
    uint32_t address = parse_uint(parser.get<std::string>("address"));
    int length = parser.get<int>("length");
    if (operation == "r" || operation == "read") {
        std::vector<uint8_t> mem_data;
        CHECK_ERRS(backend_->read_mem(address, length, mem_data));
        int wpl = 4, bpw = 4;
        if(parser.get<bool>("bytes")) {
            wpl = 16; bpw = 1;
        }
        bool ascii_view = parser.get<bool>("ascii");
        log_->info(strfmt("Read %d bytes from address 0x%08X:", length, address) + "\n" + hexdump(mem_data, address, bpw, wpl, ascii_view));
    } 
    else if (operation == "w" || operation == "write") {
        std::vector<std::string> tokens = tokenize(parser.get<std::string>("value"), ',');
        std::vector<uint8_t> mem_data;
        for (const auto &token : tokens) {
            mem_data.push_back(static_cast<uint8_t>(parse_uint(token)));
        }
        CHECK_ERRS(backend_->write_mem(address, mem_data));
        log_->info(strfmt("Wrote %zu bytes to address 0x%08X", mem_data.size(), address));
    }
    else if (operation == "loadbin") {
        uint32_t address = parse_uint(parser.get<std::string>("address"));
        std::string filepath = parser.get<std::string>("value");
        std::ifstream bin_file(filepath, std::ios::binary);
        if (!bin_file.is_open()) {
            log_->error("Failed to open binary file: " + filepath);
            return 1;
        }
        std::vector<uint8_t> mem_data((std::istreambuf_iterator<char>(bin_file)), std::istreambuf_iterator<char>());
        bin_file.close();
        CHECK_ERRS(backend_->write_mem(address, mem_data));
        log_->info(strfmt("Loaded binary file '%s' (%zu bytes) into memory at address 0x%08X", filepath.c_str(), mem_data.size(), address));
    }
    else {
        log_->error("Invalid operation. See 'help mem' for usage.");
        return 1;
    }
    return 0;
}

int VortexDebugger::cmd_dmreg(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("dmreg", "Debug module register operations");
    parser.add_argument({"operation"}, "Operation: read or write", ArgParse::STR, "", true, "", {"r", "read", "w", "write"});
    parser.add_argument({"name"}, "Register name", ArgParse::STR, "");
    parser.add_argument({"value"}, "Value to write (for write operations)", ArgParse::STR, "");

    int rc = parser.parse_args(args);
    if (rc != 0) return rc;

    std::string operation = parser.get<std::string>("operation");
    std::string name = parser.get<std::string>("name");
    std::string value = parser.get<std::string>("value");

    if (operation == "r" || operation == "read") {
        uint32_t reg_value;
        auto dmreg_id = get_dmreg_id(name);
        CHECK_ERRS(backend_->dmreg_rd(dmreg_id, reg_value));
        log_->info(strfmt("Rd DM[%s]: 0x%08X", name.c_str(), reg_value));
    }
    else if (operation == "w" || operation == "write") {
        uint32_t reg_value = parse_uint(value);
        auto dmreg_id = get_dmreg_id(name);
        CHECK_ERRS(backend_->dmreg_wr(dmreg_id, reg_value));
        log_->info(strfmt("Wr DM[%s]: 0x%08X", name.c_str(), reg_value));
    } 
    else {
        log_->error("Invalid operation. See 'help dmreg' for usage.");
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
        backend_->set_breakpoint(parse_uint(address));        
    } 
    else if (operation == "del") {
        if (address.empty()) {
            log_->error("Address required for del operation");
            return 1;
        }
        backend_->remove_breakpoint(parse_uint(address));
    } 
    else if (operation == "ls") {
        std::unordered_map<uint32_t, BreakPointInfo_t> baddrs = backend_->get_breakpoints();
        log_->info("Current breakpoints:");
        for (const auto& [addr, info] : baddrs) {
            log_->info(strfmt(" - 0x%08X : instr=0x%08X", addr, info.replaced_instr));
        }
    } 
    else {
        log_->error("Invalid operation. See 'help break' for usage.");
        return 1;
    }
    return 0;
}

int VortexDebugger::cmd_gdbserver(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("gdbserver", "Start GDB server for remote debugging");
    parser.add_argument({"--port"}, "Port to listen on", ArgParse::INT, "3333");
    int rc = parser.parse_args(args);
    if (rc != 0) return rc;

    int port = parser.get<int>("port");
    
    // Start GDB server
    GDBStub gdbstub(this, backend_);
    rc = gdbstub.serve_forever(port);
    if (rc != RCODE_OK) {
        log_->error("Failed to start GDB server on port " + std::to_string(port));
        return rc;
    }   
    return 0;
}

int VortexDebugger::cmd_param(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("param", "Get/Set debugger parameters");
    parser.add_argument({"operation"}, "Operation: get or set", ArgParse::STR, "", true, "", {"get", "set"});
    parser.add_argument({"param_name"}, "Parameter name", ArgParse::STR, "");
    parser.add_argument({"param_value"}, "Parameter value (for set operation)", ArgParse::STR, "");

    int rc = parser.parse_args(args);
    if (rc != 0) return rc;

    std::string operation = parser.get<std::string>("operation");
    std::string param_name = parser.get<std::string>("param_name");
    std::string param_value = parser.get<std::string>("param_value");

    if (operation == "get") {
        if (param_name.empty()) {
            log_->error("Parameter name required for get operation");
            return 1;
        }
        std::string param_value = backend_->get_param(param_name);
        log_->info(strfmt("Parameter %s = %s", param_name.c_str(), param_value.c_str()));
    }
    else if (operation == "set") {
        if (param_name.empty() || param_value.empty()) {
            log_->error("Parameter name and value required for set operation");
            return 1;
        }
        backend_->set_param(param_name, param_value);
    } 
    else {
        log_->error("Invalid operation. See 'help param' for usage.");
        return 1;
    }
    return 0;
}
