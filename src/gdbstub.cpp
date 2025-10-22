#include "gdbstub.h"
#include "backend.h"
#include "logger.h"
#include "tcputils.h"
#include "util.h"

#include <algorithm>

#define RECV_BUFSZ 4096

// Helpers for packet handling
std::string checksum_str(const std::string& msg) {
    uint32_t sum = 0;
    for (char c : msg) 
        sum += static_cast<uint32_t>(c);
    return strfmt("%02x", sum & 0xFF);
}

std::string packetify(const std::string& msg) {
    return strfmt("$%s#%s", msg.c_str(), checksum_str(msg).c_str());
}


GDBStub::GDBStub(Backend* backend):
    backend_(backend), 
    log_(new Logger("GDBStub", 4)),
    server_(new TCPServer()) 
{
    // Register command handlers
    cmd_map_["?"]               = &GDBStub::cmd_halted;
    cmd_map_["D"]               = &GDBStub::cmd_detach;
    cmd_map_["g"]               = &GDBStub::cmd_read_regs;
    cmd_map_["G"]               = &GDBStub::cmd_write_regs;
    cmd_map_["p"]               = &GDBStub::cmd_read_reg;
    cmd_map_["P"]               = &GDBStub::cmd_write_reg;
    // cmd_map_["m"]               = &GDBStub::cmd_read_mem;
    // cmd_map_["M"]               = &GDBStub::cmd_write_mem;
    cmd_map_["c"]               = &GDBStub::cmd_continue;
    cmd_map_["s"]               = &GDBStub::cmd_step;
    // cmd_map_["Z"]               = &GDBStub::cmd_insert_bp;
    // cmd_map_["z"]               = &GDBStub::cmd_remove_bp;
    // cmd_map_["k"]               = &GDBStub::cmd_kill;
    // cmd_map_["Hc"]              = &GDBStub::cmd_thread_select;
    // cmd_map_["Hg"]              = &GDBStub::cmd_thread_select;
    // cmd_map_["vCont?"]          = &GDBStub::cmd_vcont_query;
    cmd_map_["qSupported"]      = &GDBStub::cmd_supported;
    cmd_map_["qAttached"]       = &GDBStub::cmd_attached;
    cmd_map_["vMustReplyEmpty"] = &GDBStub::cmd_notfound;

    // Build thread map
    for(int wid=0; wid < backend_->get_num_warps(); ++wid) {
        int num_threads = backend_->get_num_threads_per_warp();
        for(int l_tid=0; l_tid < num_threads; ++l_tid) {
            int global_tid = wid * num_threads + l_tid;
            thread_map_[global_tid] = std::make_pair(wid, l_tid);
        }
    }
}

GDBStub::~GDBStub() {
    delete log_;
    delete server_;
}

int GDBStub::serve_forever(int port, bool allow_reconnect) {
    try {
        server_->start(port);
        log_->info(strfmt("GDB server listening on port %d", port));
    } 
    catch (const std::exception& e) {
        log_->error(std::string("Failed to start GDB server: ") + e.what());
        return RCODE_ERROR;
    }

    while(true) {
        log_->info("Waiting for GDB connection...");
        server_->accept_client();

        while (true) {
            std::string pkt;
            int rc = recv_packet(pkt);
            if(rc == RCODE_TRANSPORT_ERR)
                break;
            if (rc != RCODE_OK)
                continue;

            if (pkt.empty())    // empty packets: eg: due to ACK/NACK
                continue;
            
            // strip $ and #xx
            std::string cmdstr = pkt.substr(1, pkt.length() - 4);
            
            // Dispatch
            bool cmd_found = false;
            for (const auto& [prefix, fn] : cmd_map_) {
                if (cmdstr.rfind(prefix, 0) == 0) { // starts_with
                    log_->debug("Cmd: " + cmdstr);
                    send_ack();
                    (this->*fn)(cmdstr);
                    cmd_found = true;
                }
            }
            if (!cmd_found) {
                send_ack();
                cmd_notfound(cmdstr);
            }
        }

        if(!allow_reconnect) {
            log_->info("Exiting GDB server.");
            break;
        }
        log_->info("GDB client disconnected, waiting for new connection...");
    }
    return RCODE_OK;
}

int GDBStub::recv_packet(std::string& out) {
    out.clear();
    try {
        std::string buf;
        buf.reserve(RECV_BUFSZ);

        // Get first char
        char c;
        if (server_->recv_data(&c, 1) <= 0)
            return RCODE_ERROR;

        if(c == '+') {
            log_->debug("RX: + (ACK)");
            return RCODE_OK;
        }
        if(c == '-') {
            log_->warn("RX: - (NACK)");
            return RCODE_ERROR;
        }
        if(c == '\x03') {
            cmd_halted("");
            return RCODE_OK;
        }
        if (c != '$') {
            log_->warn(strfmt("RX: Unexpected start char: 0x%02X (%c)", static_cast<uint8_t>(c), c));
            return RCODE_ERROR;
        }

        uint8_t calculated_checksum = 0;
        while(true) {
            if (server_->recv_data(&c, 1) <= 0) {
                log_->warn("RX: Failed to read packet");
                return RCODE_ERROR;
            }
           
            calculated_checksum += static_cast<uint8_t>(c);
            if(buf.size() >= RECV_BUFSZ - 1) {
                log_->warn("RX: Packet too long, discarding");
                return RCODE_ERROR;
            }
            else {
                buf += c;
            }
            if (c == '#') break;
        }
        calculated_checksum -= static_cast<uint8_t>('#');
        char checksum_buf[2];
        if (server_->recv_data(checksum_buf, 2) <= 0) {
            log_->warn("Failed to read checksum");
            return RCODE_ERROR;
        }
        uint8_t received_checksum = static_cast<uint8_t>(strtol(checksum_buf, nullptr, 16));
        buf.append(checksum_buf, 2);
        log_->debug("RX: " + buf);

        if (calculated_checksum != received_checksum) {
            log_->warn(strfmt("RX: Checksum mismatch: calculated 0x%02X, received 0x%02X",
                            calculated_checksum, received_checksum));
            return RCODE_ERROR;
        }
        out = "$" + buf;
        return RCODE_OK;
    }
    catch (const std::exception& e) {
        log_->error(std::string("Failed to receive packet: ") + e.what());
        return RCODE_TRANSPORT_ERR;
    }
    return RCODE_OK;
}

void GDBStub::send_packet(const std::string& msg) {
    std::string pkt = packetify(msg);
    try {
        server_->send_data(pkt.c_str(), pkt.size());
        log_->debug("TX: " + pkt);
    } 
    catch (const std::exception& e) {
        log_->error(std::string("Failed to send packet: ") + e.what());
    }
}

void GDBStub::send_ack() {
    try {
        server_->send_data("+", 1);
        log_->debug("TX: ACK(+)");
    } 
    catch (const std::exception& e) {
        log_->error(std::string("Failed to send ACK: ") + e.what());
    }
}


//==============================================================================
// Command Handlers
//==============================================================================
// cmd: qSupported [:gdbfeature [;gdbfeature]... ]
// desc: Advertise and request for supported features
// reply: ‘PacketSize=xxxx[;gdbfeature[;gdbfeature]...]’
void GDBStub::cmd_supported(const std::string& cmdstr) {
    std::string args = cmdstr.substr(11); // skip "qSupported:"
    std::vector<std::string> features = tokenize(args, ';');

    std::string reply = "PacketSize=4096;";
    if(std::find(features.begin(), features.end(), "hwbreak+") != features.end()) {
        reply += "hwbreak+;";
    }
    send_packet(reply);
}

// cmd: qAttached:pid
// desc: Check if the remote server is attached to a running program
// reply: '1' if attached to running program, '0' if started by gdb
void GDBStub::cmd_attached(const std::string& cmdstr) {
    send_packet("1");
    is_attached_ = true;
}

// cmd: ?
// desc: Query the reason behind the target halt
// Reply: Signal that caused the target to stop
void GDBStub::cmd_halted(const std::string& cmdstr) {
    // For now, always report SIGTRAP
    send_packet("S05");
}

// cmd: D:pid
// desc: Detach the remote server from the target
// reply: OK if successful
void GDBStub::cmd_detach(const std::string& cmdstr) {
    is_attached_ = false;
    backend_->resume_warps();   // resume all warps on detach
    send_packet("OK");
}

// cmd: notfound
// desc: Default handler for unimplemented commands
void GDBStub::cmd_notfound(const std::string& cmdstr) {
    if (cmdstr != "vMustReplyEmpty") {
        log_->warn("Received unknown command: " + cmdstr + ", Skipping...");
    }    
    send_packet("");
}

// cmd: g 
// Read general registers
// reply: xxx... (concatenated register values, target dependent format)
void GDBStub::cmd_read_regs(const std::string& cmdstr) {
    // For now, return dummy registers (32 regs, all zero)
    std::string reply;
    for (int i = 0; i < 32; ++i) {
        uint32_t regval;
        backend_->read_gpr(i, regval);
        reply += strfmt("%08x", swap_endianess32(regval));
    }
    uint32_t pc;
    backend_->get_warp_pc(pc);
    reply += strfmt("%08x", swap_endianess32(pc));
    send_packet(reply);
}

// cmd: G xxx...
// desc: Write general registers
// reply: OK if successful
void GDBStub::cmd_write_regs(const std::string& cmdstr) {
    std::string args = cmdstr.substr(1); // skip "G"
    size_t reg_size = 8; // 8 hex chars = 32 bits
    for (size_t i = 0; i < 32; ++i) {
        std::string reg_str = args.substr(i * reg_size, reg_size);
        uint32_t regval = swap_endianess32(static_cast<uint32_t>(strtoul(reg_str.c_str(), nullptr, 16)));
        backend_->write_gpr(i, regval);
    }
    uint32_t pc = swap_endianess32(static_cast<uint32_t>(strtoul(args.substr(32 * reg_size, reg_size).c_str(), nullptr, 16)));
    backend_->set_warp_pc(pc);
    
    send_packet("OK");
}

// cmd: p reg_idx
// desc: Read a single register
// reply: xxxx...
void GDBStub::cmd_read_reg(const std::string& cmdstr) {
    std::string args = cmdstr.substr(1); // skip "p"
    uint32_t reg_idx = static_cast<uint32_t>(strtoul(args.c_str(), nullptr, 16));
    uint32_t regval = 0;
    if (reg_idx < 32) {
        backend_->read_gpr(reg_idx, regval);
    } 
    else if (reg_idx == 32) { // PC
        backend_->get_warp_pc(regval);
    }
    else {
        log_->error("Invalid register index: " + args);
        send_packet("E02");
        return;
    }
    regval = swap_endianess32(regval);
    send_packet(strfmt("%08x", regval));
}

// cmd: P reg_idx=val
// desc: Write a single register
// reply: OK if successful
void GDBStub::cmd_write_reg(const std::string& cmdstr) {
    std::string args = cmdstr.substr(1); // skip "P"
    size_t eq_pos = args.find('=');
    if (eq_pos == std::string::npos) {
        log_->error("Invalid write register command: " + cmdstr);
        send_packet("E01");
        return;
    }
    std::string reg_idx_str = args.substr(0, eq_pos);
    std::string val_str = args.substr(eq_pos + 1);

    uint32_t reg_idx = static_cast<uint32_t>(strtoul(reg_idx_str.c_str(), nullptr, 16));
    uint32_t regval = swap_endianess32(static_cast<uint32_t>(strtoul(val_str.c_str(), nullptr, 16)));

    int rc;
    if (reg_idx < 32) {
        rc = backend_->write_gpr(reg_idx, regval);
    } 
    else if (reg_idx == 32) { // PC
        rc = backend_->set_warp_pc(regval);
    }
    else {
        log_->error("Invalid register index: " + reg_idx_str);
        send_packet("E01");
        return;
    }
    send_packet(rc == RCODE_OK ? "OK" : "E03");
}

// cmd: m addr,length
// desc: Read memory
// reply: xx... (binary data as a sequence of hex digits)
void GDBStub::cmd_read_mem(const std::string& cmdstr) {
    std::string args = cmdstr.substr(1); // skip "m"
    size_t comma_pos = args.find(',');
    if (comma_pos == std::string::npos) {
        log_->error("Invalid read memory command: " + cmdstr);
        send_packet("E02");
        return;
    }
}


// cmd: c [addr]
// desc: Continue execution, optionally from address addr
// reply: Sxx (signal that caused the target to stop)
void GDBStub::cmd_continue(const std::string& cmdstr) {
    std::string args = cmdstr.substr(1); // skip "c"
    if (!args.empty()) {
        uint32_t addr = static_cast<uint32_t>(strtoul(args.c_str(), nullptr, 16));
        backend_->set_warp_pc(addr);
    }

    int selected_wid, selected_tid;
    backend_->get_selected_warp_thread(selected_wid, selected_tid, true);
    backend_->resume_warps({selected_wid});

    // For now, immediately report halt with SIGTRAP
    send_packet("S05");
}


// cmd: s [addr]
// desc: Step execution, optionally from address addr
// reply: Sxx (signal that caused the target to stop)
void GDBStub::cmd_step(const std::string& cmdstr) {
    std::string args = cmdstr.substr(1); // skip "s"
    if (!args.empty()) {
        uint32_t addr = static_cast<uint32_t>(strtoul(args.c_str(), nullptr, 16));
        backend_->set_warp_pc(addr);
    }
    backend_->step_warp();

    // After step, report halt with SIGTRAP
    send_packet("S05");
}