#include "vxdebug.h"
#include "logger.h"
#include <argparse.h>
#include "util.h"
#include "backend.h"

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
            add_history(input.c_str());
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
    
    // --- Execute ---
    std::string cmd = toks[0];
    int result = 1;
    try {
        result = execute_command(cmd, toks);
    } catch (const std::exception &e) {
        log_->error(e.what());
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
        backend_->get_selected_warp_pc(selected_pc, true);
        
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
        rc = backend_->initialize();
        if (rc != RCODE_OK) return rc;

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
    backend_->reset_platform(halt_warps);
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
        std::map<int, std::pair<bool, uint32_t>> warp_status;
        backend_->get_warp_status(warp_status, true);
        std::string status = "Warp Status:\n";
        for (const auto& [wid, status_pair] : warp_status) {
            int coreid = wid / backend_->state_.platinfo.num_warps;
            bool halted = status_pair.first;
            uint32_t pc = status_pair.second;
            status += strfmt(" (Core:%d) Warp %2d: %s%-8s%s PC=", coreid, wid, (halted ? ANSI_RED: ANSI_GRN), (halted ? "Halted" : "Running"), ANSI_RST);
            status += (halted ? strfmt("0x%08X", pc) : "?") + "\n";
        }
        log_->info(status);
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
    parser.add_argument({"-c", "--count"}, "Number of steps to execute", ArgParse::INT, "1");
    
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
    parser.add_argument({"--asm"}, "Assembly instruction to assemble and inject (alternative to raw instruction)", ArgParse::STR, "");
    parser.add_argument({"instruction"}, "32-bit instruction value (hex or decimal)", ArgParse::STR, "", true);
        
    int rc = parser.parse_args(args);
    if (rc != 0) return rc;

    // todo:
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

    return 0;
}

int VortexDebugger::cmd_mem(const std::vector<std::string>& args) {
    ArgParse::ArgumentParser parser("mem", "Memory operations");
    parser.add_argument({"operation"}, "Operation: read(r) or write(w)", ArgParse::STR, "", true, "", {"r", "w", "read", "write"});
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
        std::vector<uint8_t> mem_data(length);
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
