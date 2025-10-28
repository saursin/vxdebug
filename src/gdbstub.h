#pragma once
#include <string>
#include <unordered_map>

// Forward declarations
class GDBStub;
class Backend;
class Logger;
class TCPServer;

// fn pointer
typedef void (GDBStub::*cmd_handler_t)(const std::string&);

class GDBStub {
public:
    explicit GDBStub(Backend* backend);
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
    // void cmd_thread_select(const std::string& cmdstr);
    // void cmd_vcont_query(const std::string& cmdstr);
    // void cmd_target_xml(const std::string& cmdstr);
    void cmd_notfound(const std::string& cmdstr);

    // Internal state
    Backend* backend_;
    Logger* log_;
    TCPServer *server_;

    std::unordered_map<std::string, cmd_handler_t> cmd_map_;
    bool is_attached_ = false;

    std::unordered_map<int, std::pair<int, int>> thread_map_; // tid -> (g_wid, l_tid)
};