#include "gdbstub.h"
#include "backend.h"
#include "logger.h"
#include "tcputils.h"
#include "util.h"
#include "riscv.h"

#include <algorithm>

#define RECV_BUFSZ 4096

static const std::string target_xml = 
R"XML(<?xml version="1.0"?>
    <!DOCTYPE target SYSTEM "gdb-target.dtd">
    <target>
        <architecture>riscv:rv32</architecture>
        <feature name="org.gnu.gdb.riscv.cpu">
            <reg name="x0" bitsize="32" type="int" group="general"/>
            <reg name="x1" bitsize="32" type="int" group="general"/>
            <reg name="x2" bitsize="32" type="int" group="general"/>
            <reg name="x3" bitsize="32" type="int" group="general"/>
            <reg name="x4" bitsize="32" type="int" group="general"/>
            <reg name="x5" bitsize="32" type="int" group="general"/>
            <reg name="x6" bitsize="32" type="int" group="general"/>
            <reg name="x7" bitsize="32" type="int" group="general"/>
            <reg name="x8" bitsize="32" type="int" group="general"/>
            <reg name="x9" bitsize="32" type="int" group="general"/>
            <reg name="x10" bitsize="32" type="int" group="general"/>
            <reg name="x11" bitsize="32" type="int" group="general"/>
            <reg name="x12" bitsize="32" type="int" group="general"/>
            <reg name="x13" bitsize="32" type="int" group="general"/>
            <reg name="x14" bitsize="32" type="int" group="general"/>
            <reg name="x15" bitsize="32" type="int" group="general"/>
            <reg name="x16" bitsize="32" type="int" group="general"/>
            <reg name="x17" bitsize="32" type="int" group="general"/>
            <reg name="x18" bitsize="32" type="int" group="general"/>
            <reg name="x19" bitsize="32" type="int" group="general"/>
            <reg name="x20" bitsize="32" type="int" group="general"/>
            <reg name="x21" bitsize="32" type="int" group="general"/>
            <reg name="x22" bitsize="32" type="int" group="general"/>
            <reg name="x23" bitsize="32" type="int" group="general"/>
            <reg name="x24" bitsize="32" type="int" group="general"/>
            <reg name="x25" bitsize="32" type="int" group="general"/>
            <reg name="x26" bitsize="32" type="int" group="general"/>
            <reg name="x27" bitsize="32" type="int" group="general"/>
            <reg name="x28" bitsize="32" type="int" group="general"/>
            <reg name="x29" bitsize="32" type="int" group="general"/>
            <reg name="x30" bitsize="32" type="int" group="general"/>
            <reg name="x31" bitsize="32" type="int" group="general"/>
            <reg name="pc" bitsize="32" type="code_ptr" group="general"/>
        </feature>
        <feature name="org.vortex.debug.csr">
            <reg name="vx_num_cores" bitsize="32" type="int" group="vortex"/>
            <reg name="vx_num_warps" bitsize="32" type="int" group="vortex"/>
            <reg name="vx_num_threads" bitsize="32" type="int" group="vortex"/>
            <reg name="vx_core_id" bitsize="32" type="int" group="vortex"/>
            <reg name="vx_warp_id" bitsize="32" type="int" group="vortex"/>
            <reg name="vx_thread_id" bitsize="32" type="int" group="vortex"/>
            <reg name="vx_active_warps" bitsize="32" type="int" group="vortex"/>
            <reg name="vx_active_threads" bitsize="32" type="int" group="vortex"/>
            <reg name="vx_local_mem_base" bitsize="32" type="int" group="vortex"/>
        </feature>
    </target>
)XML";


// Exposed CSRs
constexpr RV_CSR CSR_LIST[] = {
    RV_CSR_VX_NUM_CORES,
    RV_CSR_VX_NUM_WARPS,
    RV_CSR_VX_NUM_THREADS,
    RV_CSR_VX_CORE_ID,
    RV_CSR_VX_WARP_ID,
    RV_CSR_VX_THREAD_ID,
    RV_CSR_VX_ACTIVE_WARPS,
    RV_CSR_VX_ACTIVE_THREADS,
    RV_CSR_VX_LOCAL_MEM_BASE
};


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
    cmd_map_["m"]               = &GDBStub::cmd_read_mem;
    cmd_map_["M"]               = &GDBStub::cmd_write_mem;
    cmd_map_["c"]               = &GDBStub::cmd_continue;
    cmd_map_["s"]               = &GDBStub::cmd_step;
    cmd_map_["Z"]               = &GDBStub::cmd_insert_bp;
    cmd_map_["z"]               = &GDBStub::cmd_remove_bp;
    // cmd_map_["k"]               = &GDBStub::cmd_kill;
    // cmd_map_["vCont?"]          = &GDBStub::cmd_vcont_query;

    // thread related commands
    cmd_map_["qfThreadInfo"]    = &GDBStub::cmd_thread_list_first;
    cmd_map_["qsThreadInfo"]    = &GDBStub::cmd_thread_list_next;
    cmd_map_["qThreadExtraInfo"] = &GDBStub::cmd_thread_info;
    cmd_map_["Hc"]              = &GDBStub::cmd_thread_select;  // Select thread for step/continue
    cmd_map_["Hg"]              = &GDBStub::cmd_thread_select;  // Select thread for general ops (mem/reg)
    cmd_map_["T"]               = &GDBStub::cmd_thread_alive;

    cmd_map_["qSupported"]      = &GDBStub::cmd_supported;
    cmd_map_["qAttached"]       = &GDBStub::cmd_attached;

    cmd_map_["qXfer:features:read:target.xml:"] = &GDBStub::cmd_qxfer_features_read;
    cmd_map_["vMustReplyEmpty"] = &GDBStub::cmd_notfound;

    // Build thread map
    for(int wid=0; wid < backend_->get_num_warps(); ++wid) {
        int num_threads = backend_->get_num_threads_per_warp();
        for(int l_tid=0; l_tid < num_threads; ++l_tid) {
            int global_tid = 1 + wid * num_threads + l_tid;
            thread_map_[global_tid] = std::make_pair(wid, l_tid);
        }
    }

    // Print thread map info
    std::string thread_map_info = "Thread Map (total threads: " + std::to_string(thread_map_.size()) + "):";
    for (const auto& [tid, wid_ltid] : thread_map_) {
        thread_map_info += strfmt(" tid %3d -> (wid:%3d, tid:%3d)\n", tid, wid_ltid.first, wid_ltid.second);
    }
    log_->debug(thread_map_info);
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
    std::string reply = "PacketSize=4096;"; // set max packet size
    reply += "qXfer:features:read+;"; // support feature read
    // Advertise software breakpoint support
    if(std::find(features.begin(), features.end(), "swbreak+") != features.end()) {
        reply += "swbreak+;";
    }

    send_packet(reply);
}

// cmd: qAttached:pid
// desc: Check if the remote server is attached to a running program
// reply: '1' if attached to running program, '0' if started by gdb
void GDBStub::cmd_attached(const std::string& cmdstr) {
    (void)cmdstr;
    send_packet("1");
    is_attached_ = true;
}

// cmd: ?
// desc: Query the reason behind the target halt
// Reply: Signal that caused the target to stop
void GDBStub::cmd_halted(const std::string& cmdstr) {
    (void)cmdstr;
    // For now, always report SIGTRAP
    send_packet("S05");
}

// cmd: D:pid
// desc: Detach the remote server from the target
// reply: OK if successful
void GDBStub::cmd_detach(const std::string& cmdstr) {
    (void)cmdstr;
    is_attached_ = false;
    backend_->resume_warps();   // resume all warps on detach
    send_packet("OK");
}

// cmd: g 
// Read general registers
// reply: xxx... (concatenated register values, target dependent format)
void GDBStub::cmd_read_regs(const std::string& cmdstr) {
    (void)cmdstr;
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

    uint32_t val;
    for (RV_CSR csr : CSR_LIST) {
        backend_->read_csr(csr, val);
        reply += strfmt("%08x", swap_endianess32(val));
    }
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
    else if (reg_idx >= 33 && reg_idx < 33 + std::size(CSR_LIST)) {
        RV_CSR csr = CSR_LIST[reg_idx - 33];
        backend_->read_csr(csr, regval);
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
    else if (reg_idx >= 33 && reg_idx < 33 + std::size(CSR_LIST)) {
        // Exposed CSRs -> read only
        rc = RCODE_ERROR;
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
        send_packet("E01");
        return;
    }
    std::string addr_s = args.substr(0, comma_pos);
    std::string length_s = args.substr(comma_pos + 1);
    uint32_t addr = static_cast<uint32_t>(strtoul(addr_s.c_str(), nullptr, 16));
    uint32_t length = static_cast<uint32_t>(strtoul(length_s.c_str(), nullptr, 16));
    std::vector<uint8_t> data;
    int rc = backend_->read_mem(addr, length, data);
    if (rc != RCODE_OK) {
        send_packet("E01");
        return;
    }
    // Send the memory contents as a hex string
    std::string hex_data;
    for (uint8_t byte : data) {
        hex_data += strfmt("%02x", byte);
    }
    send_packet(hex_data);
}


// cmd: M addr,length:xx...
// desc: Write memory
// reply: OK if successful
void GDBStub::cmd_write_mem(const std::string& cmdstr) {
    std::string args = cmdstr.substr(1); // skip "M"
    size_t colon_pos = args.find(':');
    if (colon_pos == std::string::npos) {
        log_->error("Invalid write memory command: " + cmdstr);
        send_packet("E01");
        return;
    }
    std::string addr_length_str = args.substr(0, colon_pos);
    std::string data_str = args.substr(colon_pos + 1);
    // Parse addr and length
    size_t comma_pos = addr_length_str.find(',');
    if (comma_pos == std::string::npos) {
        log_->error("Invalid write memory command: " + cmdstr);
        send_packet("E01");
        return;
    }
    std::string addr_s = addr_length_str.substr(0, comma_pos);
    std::string length_s = addr_length_str.substr(comma_pos + 1);
    uint32_t addr = static_cast<uint32_t>(strtoul(addr_s.c_str(), nullptr, 16));
    uint32_t length = static_cast<uint32_t>(strtoul(length_s.c_str(), nullptr, 16));

    // Convert hex string to byte vector
    std::vector<uint8_t> data;
    for (size_t i = 0; i < data_str.length(); i += 2) {
        std::string byte_str = data_str.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(strtoul(byte_str.c_str(), nullptr, 16));
        data.push_back(byte);
    }

    if (data.size() != length) {
        log_->error("Data length mismatch in write memory command");
        send_packet("E02");     // FIXME: Is this correct error code?
        return;
    }

    int rc = backend_->write_mem(addr, data);
    send_packet(rc == RCODE_OK ? "OK" : "E03");
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
    if(backend_->resume_warps({selected_wid}) != RCODE_OK) {
        log_->error("Failed to resume the selected warp/thread");
        send_packet("E01");     // FIXME: Is this correct error code?
        return;
    }
    if(backend_->until_breakpoint() != RCODE_OK) {
        log_->error("Failed to continue execution until breakpoint");
        send_packet("E01");     // FIXME: Is this correct error code?
        return;
    }

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
    if(backend_->step_warp() != RCODE_OK) {
        log_->error("Failed to step the selected warp/thread");
        send_packet("E01");         // FIXME: Is this correct error code?
        return;
    }

    // After step, report halt with SIGTRAP
    send_packet("S05");
}

// cmd: Z type,addr,kind
// desc: Insert breakpoint/watchpoint
// reply: OK if successful
void GDBStub::cmd_insert_bp(const std::string& cmdstr) {
    std::string args = cmdstr.substr(1); // skip "Z"
    size_t first_comma = args.find(',');
    size_t second_comma = args.find(',', first_comma + 1);
    if (first_comma == std::string::npos || second_comma == std::string::npos) {
        log_->error("Invalid insert breakpoint command: " + cmdstr);
        send_packet("E01");
        return;
    }
    std::string type_str = args.substr(0, first_comma);
    std::string addr_str = args.substr(first_comma + 1, second_comma - first_comma - 1);
    // std::string kind_str = args.substr(second_comma + 1); // unused for now

    uint32_t type = static_cast<uint32_t>(strtoul(type_str.c_str(), nullptr, 10));
    uint32_t addr = static_cast<uint32_t>(strtoul(addr_str.c_str(), nullptr, 16));

    if(type == 0 || type == 1) {
        int rc = backend_->set_breakpoint(addr);
        send_packet(rc == RCODE_OK ? "OK" : "E03");
    } 
    else {
        log_->error("Unsupported breakpoint type: " + type_str);
        send_packet("E02");
        return;
    }
}

// cmd: z type,addr,kind
// desc: Remove breakpoint/watchpoint
// reply: OK if successful
void GDBStub::cmd_remove_bp(const std::string& cmdstr) {
    std::string args = cmdstr.substr(1); // skip "z"
    size_t first_comma = args.find(',');
    size_t second_comma = args.find(',', first_comma + 1);
    if (first_comma == std::string::npos || second_comma == std::string::npos) {
        log_->error("Invalid remove breakpoint command: " + cmdstr);
        send_packet("E01");
        return;
    }
    std::string type_str = args.substr(0, first_comma);
    std::string addr_str = args.substr(first_comma + 1, second_comma - first_comma - 1);
    // std::string kind_str = args.substr(second_comma + 1); // unused for now

    uint32_t type = static_cast<uint32_t>(strtoul(type_str.c_str(), nullptr, 10));
    uint32_t addr = static_cast<uint32_t>(strtoul(addr_str.c_str(), nullptr, 16));

    if(type == 0 || type == 1) {
        int rc = backend_->remove_breakpoint(addr);
        send_packet(rc == RCODE_OK ? "OK" : "E03");
    } 
    else {
        log_->error("Unsupported breakpoint type: " + type_str);
        send_packet("E02");
        return;
    }
}

// cmd: qfThreadInfo
// desc: Request first batch of thread IDs
// reply: m[tid1,tid2,...] or l
void GDBStub::cmd_thread_list_first(const std::string& cmdstr) {
    (void)cmdstr;
    // Reset cursor
    thread_enum_cursor_ = 0;
    cmd_thread_list_next("");
}

// cmd: qsThreadInfo
// desc: Request next batch of thread IDs
// reply: m[tid1,tid2,...] or l
void GDBStub::cmd_thread_list_next(const std::string& cmdstr) {
    (void)cmdstr;   
    if (thread_enum_cursor_ >= thread_map_.size()) {
        send_packet("l"); // no more threads
        return;
    }

    std::string reply = "m";
    size_t count = 0;
    for (; thread_enum_cursor_ < thread_map_.size() && count < MAX_THREADS_PER_REPLY; ++thread_enum_cursor_, ++count) {
        int gtid = std::next(thread_map_.begin(), thread_enum_cursor_)->first;
        reply += strfmt("%x,", gtid);
    }

    // Remove trailing comma
    if (reply.back() == ',') {
        reply.pop_back();
    }
    send_packet(reply);
}

// cmd: qThreadExtraInfo,tid
// desc: Request extra info (like name) for a thread
// reply: string (null-terminated)
void GDBStub::cmd_thread_info(const std::string& cmdstr) {
    std::string args = cmdstr.substr(17); // skip "qThreadExtraInfo,"
    uint32_t tid = static_cast<uint32_t>(strtoul(args.c_str(), nullptr, 16));
    auto it = thread_map_.find(tid);
    if (it == thread_map_.end()) {
        log_->error("Invalid thread ID in thread info command: " + args);
        send_packet("");
        return;
    }
    // Send the thread info (name, etc.)
    uint32_t g_wid = it->second.first;
    uint32_t l_tid = it->second.second;
    bool active, halted;
    backend_->get_warp_state(g_wid, active, halted);

    std::string thread_info = strfmt("g_wid:%d,tid:%d,status: %s-%s", 
        g_wid, l_tid, active ? "active" : "inactive", halted ? "halted" : "unhalted");

    std::string hex_thread_info;
    for (char c : thread_info) {
        hex_thread_info += strfmt("%02x", static_cast<uint8_t>(c));
    }
    send_packet(hex_thread_info);
}

// cmd: qC
// desc: Request current thread ID
// reply: QCtid
void GDBStub::cmd_curr_thread(const std::string& cmdstr) {
    (void)cmdstr;   
    int selected_wid, selected_tid;
    backend_->get_selected_warp_thread(selected_wid, selected_tid, true);
    int gtid = selected_wid * backend_->get_num_threads_per_warp() + selected_tid;
    send_packet(strfmt("QC%x", gtid));
}

// cmd: Hc tid or Hg tid
// desc: Select a thread
// reply: OK if successful
void GDBStub::cmd_thread_select(const std::string& cmdstr) {
    std::string args = cmdstr.substr(2); // skip "Hc" or "Hg"
    uint32_t tid = static_cast<uint32_t>(strtoul(args.c_str(), nullptr, 16));
    auto it = thread_map_.find(tid);
    if (it == thread_map_.end()) {
        log_->error("Invalid thread ID in thread select command: " + args);
        send_packet("E01");
        return;
    }
    uint32_t g_wid = it->second.first;
    uint32_t l_tid = it->second.second;
    int rc = backend_->select_warp_thread(g_wid, l_tid);
    send_packet(rc == RCODE_OK ? "OK" : "E02");
}

// cmd: T tid
// desc: Check if a thread is alive
// reply: OK if alive, E01 if not
void GDBStub::cmd_thread_alive(const std::string& cmdstr) {
    std::string args = cmdstr.substr(1); // skip "T"
    uint32_t tid = static_cast<uint32_t>(strtoul(args.c_str(), nullptr, 16));
    auto it = thread_map_.find(tid);
    if (it == thread_map_.end()) {
        log_->error("Invalid thread ID in thread alive command: " + args);
        send_packet("E01");
        return;
    }
    bool is_active, is_halted;
    uint32_t g_wid = it->second.first;
    backend_->get_warp_state(g_wid, is_active, is_halted);
    if(is_active) {
        send_packet("OK");
    } else {
        send_packet("E01");
    }
}

// cmd: qXfer:features:read:target.xml:offset,length
// desc: Serve the target.xml contents to GDB
void GDBStub::cmd_qxfer_features_read(const std::string& cmdstr) {
    const std::string prefix = "qXfer:features:read:target.xml:";
    if (cmdstr.rfind(prefix, 0) != 0) {
        send_packet("E00");  // not recognized
        return;
    }

    // Find the offset and length part: e.g., "0,1000"
    std::string range = cmdstr.substr(prefix.size());
    size_t comma = range.find(',');
    if (comma == std::string::npos) {
        send_packet("E01");
        return;
    }

    uint32_t offset = strtoul(range.substr(0, comma).c_str(), nullptr, 16);
    uint32_t length = strtoul(range.substr(comma + 1).c_str(), nullptr, 16);

    if (offset >= target_xml.size()) {
        // 'l' means "last" chunk (nothing more to send)
        send_packet("l");
        return;
    }

    std::string chunk = target_xml.substr(offset, length);
    char marker = (offset + chunk.size() < target_xml.size()) ? 'm' : 'l';

    // Packet data = marker + chunk contents
    send_packet(std::string(1, marker) + chunk);
}


// cmd: notfound
// desc: Default handler for unimplemented commands
void GDBStub::cmd_notfound(const std::string& cmdstr) {
    if (cmdstr != "vMustReplyEmpty") {
        log_->warn("Received unknown command: " + cmdstr + ", Skipping...");
    }    
    send_packet("");
}