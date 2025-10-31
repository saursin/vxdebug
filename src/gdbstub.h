#pragma once
#include <string>
#include <unordered_map>
#include <map>

// Forward declarations
class GDBStub;
class VortexDebugger;
class Backend;
class Logger;
class TCPServer;

#define MAX_THREADS_PER_REPLY 64

// fn pointer
typedef void (GDBStub::*cmd_handler_t)(const std::string&);

class GDBStub {
public:
    explicit GDBStub(VortexDebugger* vxdebug, Backend* backend);
    ~GDBStub();

    int serve_forever(int port, bool allow_reconnect=true);  // Main loop

private:
    // Core helpers
    int recv_packet(std::string& out);
    void send_packet(const std::string& msg);
    void send_ack();

    // Command handlers
    void cmd_supported(const std::string& cmdstr);
    void cmd_attached(const std::string& cmdstr);
    void cmd_halted(const std::string& cmdstr);
    void cmd_detach(const std::string& cmdstr);
    void cmd_read_regs(const std::string& cmdstr);
    void cmd_write_regs(const std::string& cmdstr);
    void cmd_read_reg(const std::string& cmdstr);
    void cmd_write_reg(const std::string& cmdstr);
    void cmd_read_mem(const std::string& cmdstr);
    void cmd_write_mem(const std::string& cmdstr);
    void cmd_continue(const std::string& cmdstr);
    void cmd_step(const std::string& cmdstr);
    void cmd_insert_bp(const std::string& cmdstr);
    void cmd_remove_bp(const std::string& cmdstr);
    // void cmd_kill(const std::string& cmdstr);
    
    // void cmd_vcont_query(const std::string& cmdstr);
    // void cmd_target_xml(const std::string& cmdstr);

    void cmd_thread_list_first(const std::string& cmdstr);
    void cmd_thread_list_next(const std::string& cmdstr);
    void cmd_thread_info(const std::string& cmdstr);
    void cmd_curr_thread(const std::string& cmdstr);
    void cmd_thread_select(const std::string& cmdstr);
    void cmd_thread_alive(const std::string& cmdstr);

    void cmd_qxfer_features_read(const std::string& cmdstr);
    void cmd_monitor(const std::string& cmdstr);

    void cmd_notfound(const std::string& cmdstr);

    // Internal state
    VortexDebugger* vxdebug_;
    Backend* backend_;
    Logger* log_;
    TCPServer *server_;

    std::unordered_map<std::string, cmd_handler_t> cmd_map_;
    bool is_attached_ = false;

    std::map<int, std::pair<int, int>> thread_map_; // tid -> (g_wid, l_tid)
    size_t thread_enum_cursor_ = 0;
};