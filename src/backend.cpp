#include "backend.h"
#include "transport.h"
#include "logger.h"
#include "util.h"
#include <unistd.h>

#include "rvdefs.h"

#ifndef VORTEX_PLATFORMID
    #define VORTEX_PLATFORMID 0x1
#endif

#ifndef DMWAKE_ATTEMPT_RETRIES
    #define DMWAKE_ATTEMPT_RETRIES 3
#endif

#define msleep(x) usleep(x * 1000)

Backend::Backend(): 
    transport_(nullptr),
    transport_type_(""),
    log_(new Logger("Backend", 4))
{}

Backend::~Backend() {
    if (transport_) 
        delete transport_;
    if (log_)
        delete log_;
}

#define CHECK_ERR(stmt, msg) \
    do { \
        int rc = stmt; \
        if (rc != RCODE_OK) { \
            log_->error(std::string(msg) + "  (rc=" + std::to_string(rc) + ")"); \
            return rc; \
        } \
    } while(0)

#define CHECK_TRANSPORT() \
    if(!transport_ || !transport_->is_connected()) { \
        log_->error("Transport uninitialized or disconnected"); \
        return RCODE_TRANSPORT_ERR; \
    }

#define CHECK_SELECTED() \
    if (state_.selected_wid < 0 || state_.selected_tid < 0) { \
        log_->error("No warp/thread selected"); \
        return RCODE_NONESELECTED_ERR; \
    }

//==============================================================================
// Transport management
//==============================================================================

int Backend::transport_setup(const std::string &type) {
    if (transport_) {
        log_->warn("Transport already set up. Destroying existing transport.");
        delete transport_;
        transport_ = nullptr;
        transport_type_ = "";
    }

    transport_type_ = type;

    if (type == "tcp") {
        transport_ = new TCPTransport();
        log_->debug("TCP transport created");
    } else {
        log_->error("Unknown transport type: " + type);
        return RCODE_INVALID_ARG;
    }
    return RCODE_OK;
}

int Backend::transport_connect(const std::map<std::string, std::string> &args) {
    if(!transport_) {
        log_->error("INTERNAL ERROR: Transport not set up. Call transport_setup() first.");
        return RCODE_TRANSPORT_ERR;
    }

    if (transport_type_ == "tcp") {
        CHECK_ERR(transport_->connect(args), "Failed to connect TCP transport");
        log_->info("Transport connected!");
    } else {
        log_->error("Transport type not supported for connection: " + transport_type_);
        return RCODE_INVALID_ARG;
    }
    return RCODE_OK;
}

int Backend::transport_disconnect() {
    if (!transport_) {
        log_->warn("Transport not set up, nothing to disconnect.");
        return RCODE_OK;
    }
    
    CHECK_ERR(transport_->disconnect(), "Failed to disconnect transport");

    log_->info("Transport disconnected!");
    return RCODE_OK;
}

bool Backend::transport_connected() const {
    return transport_ && transport_->is_connected();
}


//==============================================================================
// Initialization
//==============================================================================

int Backend::initialize() {
    CHECK_TRANSPORT();
    log_->info("Initializing backend...");

    // Try to Wake DM
    CHECK_ERR(wake_dm(), "Failed to wake DM");

    // Get platform info
    CHECK_ERR(fetch_platform_info(), "Failed to fetch platform info");

    log_->info("Backend initialized!");

    // Print platform info
    _print_platform_info();
    return RCODE_OK;
}


//==============================================================================
// API Methods
//==============================================================================

////////////////////////////////////////////////////////////////////////////////
// Warp Control Methods

int Backend::wake_dm() {  
    // Check if ndmreset is set
    uint32_t ndmreset = 0;
    CHECK_ERR(dmreg_rdfield(DMReg_t::DCTRL, "ndmreset", ndmreset), "Failed to read DCTRL.ndmreset field");

    if (ndmreset) {
        // Wait for ndmreset to low
        log_->debug("Waiting for DCTRL.ndmreset to clear...");
        CHECK_ERR(dmreg_pollfield(DMReg_t::DCTRL, "ndmreset", 0, &ndmreset), "Failed to poll DCTRL.ndmreset field");
    }

    // Check if dm is active
    uint32_t dmactive = 0;
    CHECK_ERR(dmreg_rdfield(DMReg_t::DCTRL, "dmactive", dmactive), "Failed to read DCTRL.dmactive field");
    if (!dmactive) {
        // DM is not active, need to wake it up
        log_->debug("DM not active, Waking up DM by setting DCTRL.dmactive...");
        int dmwake_attempt_retries = DMWAKE_ATTEMPT_RETRIES;
        while(!dmactive && dmwake_attempt_retries-- > 0) {
            CHECK_ERR(dmreg_wrfield(DMReg_t::DCTRL, "dmactive", 1), 
                "Failed to write DCTRL.dmactive field");
            
            CHECK_ERR(dmreg_pollfield(DMReg_t::DCTRL, "dmactive", 1, &dmactive),
                "Failed to poll DCTRL.dmactive field");
        }
    }
    if (!dmactive) {
        log_->error("Failed to wake DM after multiple attempts");
        return RCODE_ERROR;
    }
    log_->debug("DM is awake!");
    return RCODE_OK;
}

int Backend::reset(bool halt_warps) {
    // Issue system reset via DCTRL.ndmreset
    log_->info("Issuing system reset...");
    if (halt_warps) {
        // Select all warps to halt
        log_->debug("Selecting all warps to halt after reset.");
        CHECK_ERR(select_warps(true), "Failed to select all warps for halting after reset");

        // Set resethaltreq -> triggers halt after reset for all selected warps
        CHECK_ERR(dmreg_wrfield(DMReg_t::DCTRL, "resethaltreq", 1), "Failed to set DCTRL.resethaltreq field");
    }

    // Issue reset
    log_->debug("Setting DCTRL.ndmreset to initiate reset.");
    CHECK_ERR(dmreg_wrfield(DMReg_t::DCTRL, "ndmreset", 1), "Failed to set DCTRL.ndmreset field");

    // Wait for reset to complete (ndmreset to clear)
    log_->debug("Waiting for reset to complete... (DCTRL.ndmreset to clear)");
    uint32_t ndmreset = 1;
    CHECK_ERR(dmreg_pollfield(DMReg_t::DCTRL, "ndmreset", 0, &ndmreset), "Failed to poll DCTRL.ndmreset field after reset");

    // Pre-read DCTRL to check warp halt status
    log_->debug("Checking warp halt status after reset.");
    uint32_t dctrl;
    CHECK_ERR(dmreg_rd(DMReg_t::DCTRL, dctrl), 
        "Failed to read DCTRL register");

    if(halt_warps) {
        if(extract_dmreg_field(DMReg_t::DCTRL, "allhalted", dctrl)) {
            log_->info("All warps halted after reset.");
        }
        else if(extract_dmreg_field(DMReg_t::DCTRL, "anyhalted", dctrl)) {
            log_->warn("Some warps halted after reset, but not all.");
        }
        else {
            log_->error("No warps halted after reset.");
        }
    }

    log_->info("System reset complete.");
    
    // Re-initialize backend after reset
    CHECK_ERR(initialize(), "Failed to re-initialize backend after reset");

    return RCODE_OK;
}

int Backend::fetch_platform_info() {
    uint32_t platform = 0;
    CHECK_ERR(dmreg_rd(DMReg_t::PLATFORM, platform), "Failed to read PLATFORM register");
    
    state_.platinfo.platform_id     = extract_dmreg_field(DMReg_t::PLATFORM, "platformid", platform);
    state_.platinfo.platform_name   = state_.platinfo.platform_id == VORTEX_PLATFORMID ? "Vortex" : "Unknown";
    state_.platinfo.num_clusters    = extract_dmreg_field(DMReg_t::PLATFORM, "numclusters", platform);
    state_.platinfo.num_cores       = extract_dmreg_field(DMReg_t::PLATFORM, "numcores", platform);
    state_.platinfo.num_warps       = extract_dmreg_field(DMReg_t::PLATFORM, "numwarps", platform);
    state_.platinfo.num_threads     = extract_dmreg_field(DMReg_t::PLATFORM, "numthreads", platform);
    state_.platinfo.num_total_cores = state_.platinfo.num_clusters * state_.platinfo.num_cores;
    state_.platinfo.num_total_warps = state_.platinfo.num_total_cores * state_.platinfo.num_warps;
    state_.platinfo.num_total_threads = state_.platinfo.num_total_warps * state_.platinfo.num_threads;
    return RCODE_OK;
}

////////////////////////////////////////////////////////////////////////////////
// Warp Selection

int Backend::select_warps(const std::vector<int> &wids) {
    // Create enough 32-bit masks to cover all warps
    size_t num_win = (state_.platinfo.num_total_warps + 31) / 32;
    std::vector<uint32_t> win_masks(num_win, 0);
    
    // Set bits for each selected warp ID
    for (int wid : wids) {
        if (wid < 0 || wid >= static_cast<int>(state_.platinfo.num_total_warps)) {
            log_->warn("Ignoring invalid warp ID " + std::to_string(wid));
            continue;
        }
        int win_idx = wid / 32;
        int bit_pos  = wid % 32;
        win_masks[win_idx] |= (1u << bit_pos);
    }

    // Write the masks to the DM registers
    for(size_t i = 0; i < num_win; ++i) {
        // Select the window
        CHECK_ERR(dmreg_wrfield(DMReg_t::DSELECT, "winsel", i), "Failed to write DSELECT.winsel field");

        // Write the mask
        CHECK_ERR(dmreg_wrfield(DMReg_t::WMASK, "mask", win_masks[i]), "Failed to write WMASK.mask field");
    }
    log_->info("Selected " + std::to_string(wids.size()) + " warps.");
    return RCODE_OK;
}

int Backend::select_warps(bool all) {
    size_t num_win = (state_.platinfo.num_total_warps + 31) / 32;
    for (size_t i = 0; i < num_win; ++i) {
        CHECK_ERR(dmreg_wrfield(DMReg_t::DSELECT, "winsel", i), "Failed to write DSELECT.winsel field");

        CHECK_ERR(dmreg_wrfield(DMReg_t::WMASK, "mask", all ? 0xFFFFFFFF : 0x00000000), "Failed to write WMASK.mask field");
    }
    return RCODE_OK;
}

int Backend::select_warp_thread(int g_wid, int tid) {
    if (g_wid < 0 || g_wid >= static_cast<int>(state_.platinfo.num_total_warps)) {
        log_->error("Invalid global warp ID " + std::to_string(g_wid));
        return RCODE_INVALID_ARG;
    }
    if (tid < 0 || tid >= static_cast<int>(state_.platinfo.num_threads)) {
        log_->error("Invalid thread ID " + std::to_string(tid));
        return RCODE_INVALID_ARG;
    }
    uint32_t dselect = 0;
    CHECK_ERR(dmreg_rd(DMReg_t::DSELECT, dselect), "Failed to read DSELECT register");
    

    dselect = set_dmreg_field(DMReg_t::DSELECT, "warpsel", dselect, g_wid);
    dselect = set_dmreg_field(DMReg_t::DSELECT, "threadsel", dselect, tid);
    CHECK_ERR(dmreg_wr(DMReg_t::DSELECT, dselect), "Failed to write DSELECT register");
    state_.selected_wid = g_wid;
    state_.selected_tid = tid;
    log_->info("Selected warp " + std::to_string(g_wid) + ", thread " + std::to_string(tid) + " for debugging.");
    return RCODE_OK;
}

int Backend::get_selected_warp_thread(int &wid, int &tid, bool force_fetch) {
    if (force_fetch) {
        uint32_t dselect = 0;
        CHECK_ERR(dmreg_rd(DMReg_t::DSELECT, dselect), "Failed to read DSELECT register");
        state_.selected_wid = static_cast<int>(extract_dmreg_field(DMReg_t::DSELECT, "warpsel", dselect));
        state_.selected_tid = static_cast<int>(extract_dmreg_field(DMReg_t::DSELECT, "threadsel", dselect));
    }

    wid = state_.selected_wid;
    tid = state_.selected_tid;
    return RCODE_OK;
}


////////////////////////////////////////////////////////////////////////////////
// Query Warp Status

int Backend::get_warp_status(std::map<int, std::pair<bool, uint32_t>> &warp_status, bool include_pc) {
    warp_status.clear();

    // Save current selection
    int saved_wid = state_.selected_wid;
    int saved_tid = state_.selected_tid;

    size_t num_wins = (state_.platinfo.num_total_warps + 31) / 32;
    for (size_t win=0; win < num_wins; ++win) {
        // Select win
        CHECK_ERR(dmreg_wrfield(DMReg_t::DSELECT, "winsel", win), "Failed to write DSELECT.winsel field");

        // Read WSTATUS
        uint32_t wstatus = 0;
        CHECK_ERR(dmreg_rd(DMReg_t::WSTATUS, wstatus), "Failed to read WSTATUS register");
        
        // Parse status bits
        for (size_t bit=0; bit < 32; ++bit) {
            int wid = win * 32 + bit;
            if (wid >= static_cast<int>(state_.platinfo.num_total_warps))
                break;
            bool halted = (wstatus >> bit) & 0x1;
            uint32_t pc = 0;
            if(halted && include_pc) {
                select_warp_thread(wid, 0);     // Changes current selection
                CHECK_ERR(dmreg_rd(DMReg_t::DPC, pc), "Failed to read DPC register for warp " + std::to_string(wid));
            }
            warp_status[wid] = std::make_pair(halted, pc);
        }
    }
    
    // Restore original selection
    if (saved_wid >= 0 && saved_tid >= 0) {
        CHECK_ERR(select_warp_thread(saved_wid, saved_tid), "Failed to restore original warp/thread selection");
    }
    return RCODE_OK;
}

int Backend::get_warp_summary(bool *allhalted, bool *anyhalted, bool *allrunning, bool *anyrunning) {
    uint32_t dctrl = 0;
    CHECK_ERR(dmreg_rd(DMReg_t::DCTRL, dctrl), "Failed to read DCTRL register");
    if(allhalted) {
        *allhalted = extract_dmreg_field(DMReg_t::DCTRL, "allhalted", dctrl) ? true : false;
    }
    if(anyhalted) {
        *anyhalted = extract_dmreg_field(DMReg_t::DCTRL, "anyhalted", dctrl) ? true : false;
    }
    if(allrunning) {
        *allrunning = extract_dmreg_field(DMReg_t::DCTRL, "allrunning", dctrl) ? true : false;
    }
    if(anyrunning) {
        *anyrunning = extract_dmreg_field(DMReg_t::DCTRL, "anyrunning", dctrl) ? true : false;
    }
    return RCODE_OK;
}

int Backend::get_warp_pc(uint32_t &pc) {
    CHECK_SELECTED();
    CHECK_ERR(dmreg_rd(DMReg_t::DPC, pc), "Failed to read DPC register");
    return RCODE_OK;
}

int Backend::set_warp_pc(const uint32_t pc) {
    CHECK_SELECTED();
    CHECK_ERR(dmreg_wr(DMReg_t::DPC, pc), 
        "Failed to write DPC register");
    return RCODE_OK;
}


////////////////////////////////////////////////////////////////////////////////
// Warp Control Methods

int Backend::halt_warps(const std::vector<int> &wids) {
    log_->info("Halting warps: " + std::to_string(wids.size()) + " warps");
    
    // Select the specified warps
    CHECK_ERR(select_warps(wids), "Failed to select warps for halting");

    // Send halt request
    CHECK_ERR(dmreg_wrfield(DMReg_t::DCTRL, "haltreq", 1), "Failed to send halt request");
    
    // Check halt completion
    uint32_t anyhalted;
    CHECK_ERR(dmreg_pollfield(DMReg_t::DCTRL, "anyhalted", 1, &anyhalted), 
        "Failed to poll halt status");
    
    log_->debug("Halt request sent for selected warps");
    return RCODE_OK;
}

int Backend::halt_warps() {
    log_->info("Halting all warps");
    
    // Select all warps
    CHECK_ERR(select_warps(true), "Failed to select all warps for halting");
    
    // Send halt request
    CHECK_ERR(dmreg_wrfield(DMReg_t::DCTRL, "haltreq", 1), "Failed to send halt request");
    
    log_->debug("Halt request sent for all warps");
    return RCODE_OK;
}

int Backend::resume_warps(const std::vector<int> &wids) {
    log_->info("Resuming warps: " + std::to_string(wids.size()) + " warps");
    
    // Select the specified warps
    CHECK_ERR(select_warps(wids), "Failed to select warps for resuming");

    // Send resume request
    CHECK_ERR(dmreg_wrfield(DMReg_t::DCTRL, "resumereq", 1), "Failed to send resume request");
    
    log_->debug("Resume request sent for selected warps");
    return RCODE_OK;
}

int Backend::resume_warps() {
    log_->info("Resuming all warps");
    
    // Select all warps
    CHECK_ERR(select_warps(true), "Failed to select all warps for resuming");

    // Send resume request
    CHECK_ERR(dmreg_wrfield(DMReg_t::DCTRL, "resumereq", 1), "Failed to send resume request");
    log_->debug("Resume request sent for all warps");
    return RCODE_OK;
}

int Backend::step_warp() {
    CHECK_SELECTED();
    bool all_halted = false;
    CHECK_ERR(get_warp_summary(&all_halted, nullptr, nullptr, nullptr), "Failed to get warp summary before stepping");
    if (all_halted) log_->warn("All warps are halted, Stepping a warp may cause deadlock.");

    // Send step request
    CHECK_ERR(dmreg_wrfield(DMReg_t::DCTRL, "stepreq", 1), "Failed to send step request");

    // Poll for step completion
    uint32_t stepstate;
    CHECK_ERR(dmreg_pollfield(DMReg_t::DCTRL, "stepstate", 0, &stepstate), "Failed to poll step state");
    
    // Read updated PC
    uint32_t warp_pc;
    CHECK_ERR(dmreg_rd(DMReg_t::DPC, warp_pc), "Failed to read DPC register after step");
    log_->info(strfmt("Stepped warp %d to PC=0x%08X", state_.selected_wid, warp_pc));
    
    return RCODE_OK;
}

int Backend::inject_instruction(uint32_t instruction) {
    CHECK_SELECTED();
       
    // Write instruction to DINJECT register
    CHECK_ERR(dmreg_wr(DMReg_t::DINJECT, instruction), 
        "Failed to write instruction to DINJECT register");
    
    // Send injection request
    CHECK_ERR(dmreg_wrfield(DMReg_t::DCTRL, "injectreq", 1), 
        "Failed to send injection request");
    
    // Poll for injection completion
    uint32_t inject_state;
    CHECK_ERR(dmreg_pollfield(DMReg_t::DCTRL, "injectstate", 0x0, &inject_state), "Failed to poll injection state");
        
    log_->debug(strfmt("Injected instr (wid: %d, tid: %d): 0x%08X", state_.selected_wid, state_.selected_tid, instruction));
    return RCODE_OK;
}


////////////////////////////////////////////////////////////////////////////////
// Platform Query/Update Methods

int reg_arch_read(const uint32_t regnum, uint32_t &value) {
    (void)regnum;
    (void)value;
    return RCODE_OK;
}

// Set the architecture register value
int reg_arch_write(const uint32_t regnum, const uint32_t value) {
    (void)regnum;
    (void)value;
    return RCODE_OK;
}

// Get the CSR register value
int reg_csr_read(const uint32_t regaddr, uint32_t &value) {
    (void)regaddr;
    (void)value;
    return RCODE_OK;
}

// Set the CSR register value
int reg_csr_write(const uint32_t regaddr, const uint32_t value) {
    (void)regaddr;
    (void)value;
    return RCODE_OK;
}

// Read processor register
int read_register(const std::string &reg_name, uint32_t &value) {
    (void)reg_name;
    (void)value;
    return RCODE_OK;
}

// Write processor register
int write_register(const std::string &reg_name, uint32_t value) {
    (void)reg_name;
    (void)value;
    return RCODE_OK;
}

// Read memory
int mem_read(const uint32_t address, const uint32_t size, std::vector<uint32_t> &data) {
    (void)address;
    (void)size;
    (void)data;
    return RCODE_OK;
}

// Write memory
int mem_write(const uint32_t address, const uint32_t size, const std::vector<uint32_t> &data) {
    (void)address;
    (void)size;
    (void)data;
    return RCODE_OK;
}


////////////////////////////////////////////////////////////////////////////////
// Helpers

void Backend::_print_platform_info() const {
    std::string info;
    info += strfmt("  Platform ID   : 0x%08X (%s)\n", state_.platinfo.platform_id, state_.platinfo.platform_name.c_str());
    info += strfmt("  Clusters      : %u\n", state_.platinfo.num_clusters);
    info += strfmt("  Cores/Cluster : %u\n", state_.platinfo.num_cores);
    info += strfmt("  Warps/Core    : %u\n", state_.platinfo.num_warps);
    info += strfmt("  Threads/Warp  : %u\n", state_.platinfo.num_threads);
    info += strfmt("  Total Cores   : %u\n", state_.platinfo.num_total_cores);
    info += strfmt("  Total Warps   : %u\n", state_.platinfo.num_total_warps);
    info += strfmt("  Total Threads : %u\n", state_.platinfo.num_total_threads);
    log_->info("Platform Information:\n" + info);
    return;
}


// =============================================================================
// Low-level DM register access
// =============================================================================
int Backend::dmreg_rd(const DMReg_t &reg, uint32_t &value) {
    CHECK_TRANSPORT();
    const auto& rinfo = get_dmreg(reg);
    CHECK_ERR(transport_->read_reg(rinfo.addr, value), "Failed to read DM register " + std::string(rinfo.name));
    log_->debug(strfmt("Rd DMReg[0x%04X, %s] => 0x%08X", rinfo.addr, std::string(rinfo.name).c_str(), value));
    return RCODE_OK;
}

int Backend::dmreg_wr(const DMReg_t &reg, const uint32_t &value) {
    CHECK_TRANSPORT();
    const auto& rinfo = get_dmreg(reg);
    CHECK_ERR(transport_->write_reg(rinfo.addr, value), "Failed to write DM register " + std::string(rinfo.name));
    log_->debug(strfmt("Wr DMReg[0x%04X, %s] <= 0x%08X", rinfo.addr, std::string(rinfo.name).c_str(), value));
    return RCODE_OK;
}

int Backend::dmreg_rdfield(const DMReg_t &reg, const std::string &fieldname, uint32_t &value) {
    CHECK_TRANSPORT();
    const auto& rinfo = get_dmreg(reg);   
    try {
        const FieldInfo_t* finfo = get_dmreg_field(reg, fieldname);
    
        uint32_t reg_value = 0;
        CHECK_ERR(transport_->read_reg(rinfo.addr, reg_value), "Failed to read DM register " + std::string(rinfo.name));
        value = extract_dmreg_field(reg, fieldname, reg_value);
        log_->debug(strfmt("Rd DMReg[0x%04X, %s.%s] => 0x%X", rinfo.addr, std::string(rinfo.name).c_str(), std::string(finfo->name).c_str(), value));
        return RCODE_OK;
    } catch (const std::exception& e) {
        log_->error("Failed to read field: " + std::string(e.what()));
        return RCODE_INVALID_ARG;
    }
    return RCODE_OK;
}

int Backend::dmreg_wrfield(const DMReg_t &reg, const std::string &fieldname, const uint32_t &value) {
    CHECK_TRANSPORT();
    const auto& rinfo = get_dmreg(reg);  
    try {
        const FieldInfo_t* finfo = get_dmreg_field(reg, fieldname);

        // Read current register value
        uint32_t curr_reg_value = 0;
        CHECK_ERR(transport_->read_reg(rinfo.addr, curr_reg_value), "Failed to read DM register " + std::string(rinfo.name));

        // Modify only the specific field
        uint32_t new_reg_value = set_dmreg_field(reg, fieldname, curr_reg_value, value);
        
        // Write back the modified register
        CHECK_ERR(transport_->write_reg(rinfo.addr, new_reg_value), "Failed to write DM register " + std::string(rinfo.name));

        log_->debug(strfmt("Wr DMReg[0x%04X, %s.%s] <= 0x%X (NewRegVal: 0x%08X, OldRegVal: 0x%08X)", 
                          rinfo.addr, 
                          std::string(rinfo.name).c_str(), 
                          std::string(finfo->name).c_str(), 
                          value, 
                          new_reg_value, 
                          curr_reg_value));
        return RCODE_OK;
    } catch (const std::exception& e) {
        log_->error("Failed to write field: " + std::string(e.what()));
        return RCODE_INVALID_ARG;
    }
}

int Backend::dmreg_pollfield(const DMReg_t &reg, const std::string &fieldname, const uint32_t &exp_value, uint32_t *final_value,
                        int max_retries, int delay_ms) {
    CHECK_TRANSPORT();
    const auto& rinfo = get_dmreg(reg);
    
    try {
        const FieldInfo_t* finfo = get_dmreg_field(reg, fieldname);

        uint32_t field_mask = finfo->mask();
        uint32_t value = 0;
    
        for (int attempt = 0; attempt < max_retries; ++attempt) {
            // Read register value
            uint32_t reg_value = 0;
            CHECK_ERR(transport_->read_reg(rinfo.addr, reg_value), "Failed to read DM register " + std::string(rinfo.name));
            
            // Extract field value
            value = (reg_value & field_mask) >> finfo->lsb;
            log_->debug("Poll DM[" + std::string(rinfo.name) + "." + std::string(finfo->name) + "] = 0x" + hex2str(value) + 
                    " (attempt " + std::to_string(attempt + 1) + "/" + std::to_string(max_retries) + ")");
            
            // Check if expected value is reached
            if (value == exp_value) {
                if (final_value) {
                    *final_value = value;
                }
                return RCODE_OK;
            }
            
            // Don't sleep on the last iteration
            if (attempt < max_retries - 1) {
                msleep(delay_ms);
            }
        }
    
        // Store final value for caller inspection
        if (final_value) {
            *final_value = value;
        }
        
        log_->error("MaxRetryReached: Field " + std::string(rinfo.name) + "." + std::string(finfo->name) +
                    " did not reach expected value 0x" + hex2str(exp_value) + 
                    " (final value: 0x" + hex2str(value) + ")");
        return RCODE_TIMEOUT;
    } catch (const std::exception& e) {
        log_->error("Failed to poll field: " + std::string(e.what()));
        return RCODE_INVALID_ARG;
    }
}
