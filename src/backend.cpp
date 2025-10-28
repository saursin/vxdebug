#include "backend.h"
#include "transport.h"
#include "logger.h"
#include "util.h"
#include <unistd.h>
#include <cstring>   // for std::memcpy
#include <cmath>    // for std::pow

#include "riscv.h"

#ifndef VORTEX_PLATFORMID
    #define VORTEX_PLATFORMID 0x1
#endif

#ifndef DMWAKE_ATTEMPT_RETRIES
    #define DMWAKE_ATTEMPT_RETRIES 3
#endif

#define msleep(x) usleep(x * 1000)

#define CHECK_ERR(stmt, msg) \
    do { \
        int rc = stmt; \
        if (rc != RCODE_OK) { \
            log_->error(std::string(msg) + "  (rc=" + std::to_string(rc) + ")"); \
            return rc; \
        } \
    } while(0)

#define CHECK_ERRS(stmt) \
    do { \
        int rc = stmt; \
        if (rc != RCODE_OK) { \
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


////////////////////////////////////////////////////////////////////////////////
// Backend
////////////////////////////////////////////////////////////////////////////////

Backend::Backend(): 
    transport_(nullptr),
    transport_type_(""),
    log_(new Logger("Backend", 4))
{
    if (!rv_toolchain_check()) {
        log_->warn("RISC-V toolchain not found in PATH. Instruction inject and all dependent functionality will not work");
    }
}

Backend::~Backend() {
    if (transport_) 
        delete transport_;
    if (log_)
        delete log_;    
}

void Backend::set_param(const std::string &param, std::string value) {
    if (param == "poll_retries") {
        poll_retries_ = std::stoul(value);
        log_->info("Set parameter poll_retries to " + value);
    } 
    else if (param == "poll_delay_ms") {
        poll_delay_ms_ = std::stoul(value);
        log_->info("Set parameter poll_delay_ms to " + value);
    }
    else {
        log_->warn("Unknown parameter: " + param);
    }
}

std::string Backend::get_param(const std::string &param) const {
    if (param == "poll_retries") {
        return std::to_string(poll_retries_);
    } 
    else if (param == "poll_delay_ms") {
        return std::to_string(poll_delay_ms_);
    }
    else {
        log_->warn("Unknown parameter: " + param);
        return "?";
    }
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

int Backend::initialize(bool quiet) {
    CHECK_TRANSPORT();
    log_->info("Initializing backend...");

    // Try to Wake DM
    log_->debug("Querying debug module status");
    CHECK_ERR(wake_dm(), "Failed to wake DM");
   
    // Get platform info
    log_->debug("Fetching platform information...");
    CHECK_ERR(fetch_platform_info(), "Failed to fetch platform info");

    log_->debug("Backend initialized!");

    // Print platform info
    if (!quiet) _print_platform_info();
    return RCODE_OK;
}


//==============================================================================
// API Methods
//==============================================================================

//----- Warp Control Methods ---------------------------------------------------
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

int Backend::reset_platform(bool halt_warps) {
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

    if(halt_warps) {
        bool allhalted, anyhalted;
        CHECK_ERR(get_warp_summary(&allhalted, &anyhalted), "Failed to get warp halt summary after reset");   // updates the cache
        if(allhalted) {
            log_->info("All warps halted after reset.");
        }
        else if(anyhalted) {
            log_->warn("Some warps halted after reset, but not all.");
        }
        else {
            log_->error("No warps halted after reset.");
        }
    }

    log_->info("System reset complete.");
    
    // Re-initialize backend after reset
    CHECK_ERR(initialize(true), "Failed to re-initialize backend after reset");
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
    state_.platinfo.num_threads     = std::pow(2, extract_dmreg_field(DMReg_t::PLATFORM, "numthreads", platform));
    state_.platinfo.num_total_cores = state_.platinfo.num_clusters * state_.platinfo.num_cores;
    state_.platinfo.num_total_warps = state_.platinfo.num_total_cores * state_.platinfo.num_warps;
    state_.platinfo.num_total_threads = state_.platinfo.num_total_warps * state_.platinfo.num_threads;

    // save curr warp/thread selection
    bool saved = false;
    int selected_wid = -1;
    int selected_tid = -1;
    if(state_.selected_wid >=0 && state_.selected_tid >=0) {
        selected_wid = state_.selected_wid;
        selected_tid = state_.selected_tid;
        saved = true;
    }

    // switch to warp 0, thread 0
    CHECK_ERR(select_warp_thread(0, 0), "Failed to select warp 0, thread 0");

    // check if warp 0 is halted
    bool w0_is_halted = false;
    CHECK_ERR(get_warp_state(0, w0_is_halted), "Failed to get warp state");

    if(!w0_is_halted) {
        // halt warp 0
        CHECK_ERR(halt_warps({0}), "Failed to halt warp 0");
    }
    
    // Obtain ISA Information
    uint32_t misa = 0;
    CHECK_ERR(read_csr(RV_CSR_MISA, misa), "Failed to read MISA CSR");
    state_.platinfo.misa = misa;

    if(!w0_is_halted) {
        // resume warp 0 if it was running before
        CHECK_ERR(resume_warps({0}), "Failed to resume warp 0");
    }

    // restore previous warp/thread selection
    if (saved) {
        CHECK_ERR(select_warp_thread(selected_wid, selected_tid), "Failed to restore previously selected warp/thread");
    }
    return RCODE_OK;
}


//----- Warp Selection ---------------------------------------------------------
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
    return RCODE_OK;
}

int Backend::select_warps(bool all) {
    size_t num_win = (state_.platinfo.num_total_warps + 31) / 32;
    for (size_t i = 0; i < num_win; ++i) {
        CHECK_ERR(dmreg_wrfield(DMReg_t::DSELECT, "winsel", i), "Failed to write DSELECT.winsel field");
        CHECK_ERR(dmreg_wr(DMReg_t::WMASK, all ? 0xFFFFFFFF : 0x00000000), "Failed to write WMASK.mask field");
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
    // uint32_t dselect = 0;
    // CHECK_ERR(dmreg_rd(DMReg_t::DSELECT, dselect), "Failed to read DSELECT register");
    // dselect = set_dmreg_field(DMReg_t::DSELECT, "warpsel", dselect, g_wid);
    // dselect = set_dmreg_field(DMReg_t::DSELECT, "threadsel", dselect, tid);
    // CHECK_ERR(dmreg_wr(DMReg_t::DSELECT, dselect), "Failed to write DSELECT register");

    CHECK_ERR(dmreg_wrfield(DMReg_t::DSELECT, "warpsel", g_wid), "Failed to write DSELECT.warpsel field");
    CHECK_ERR(dmreg_wrfield(DMReg_t::DSELECT, "threadsel", tid), "Failed to write DSELECT.threadsel field");

    state_.selected_wid = g_wid;
    state_.selected_tid = tid;
    
    // Update cached PC for the newly selected warp
    uint32_t pc;
    CHECK_ERR(dmreg_rd(DMReg_t::DPC, pc), "Failed to read DPC register for newly selected warp");
    state_.selected_warp_pc = pc;

    log_->debug("Selected warp " + std::to_string(g_wid) + ", thread " + std::to_string(tid) + " for debugging.");
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

//----- Query Warp Status ------------------------------------------------------

int Backend::get_warp_status(std::map<int, WarpStatus_t> &warp_status, bool include_pc, bool include_hacause) {
    warp_status.clear();

    // Save current selection
    int saved_wid = state_.selected_wid;
    int saved_tid = state_.selected_tid;

    size_t num_wins = (state_.platinfo.num_total_warps + 31) / 32;
    for (size_t win=0; win < num_wins; ++win) {
        // Select win
        CHECK_ERR(dmreg_wrfield(DMReg_t::DSELECT, "winsel", win), "Failed to write DSELECT.winsel field");

        // Read Wactive
        uint32_t wactive = 0;
        CHECK_ERR(dmreg_rd(DMReg_t::WACTIVE, wactive), "Failed to read WACTIVE register");

        // Read WSTATUS
        uint32_t wstatus = 0;
        CHECK_ERR(dmreg_rd(DMReg_t::WSTATUS, wstatus), "Failed to read WSTATUS register");
        
        // Parse status bits
        for (size_t bit=0; bit < 32; ++bit) {
            int wid = win * 32 + bit;
            if (wid >= static_cast<int>(state_.platinfo.num_total_warps))
                break;
            bool active = (wactive >> bit) & 0x1;
            bool halted = (wstatus >> bit) & 0x1;
            uint32_t pc = 0;
            uint32_t hacause = 0;
            if(active && halted && include_pc) {
                select_warp_thread(wid, 0);     // Changes current selection
                CHECK_ERR(dmreg_rd(DMReg_t::DPC, pc), "Failed to read DPC register for warp " + std::to_string(wid));
            }
            if(active && halted && include_hacause) {
                select_warp_thread(wid, 0);     // Changes current selection
                CHECK_ERR(dmreg_rdfield(DMReg_t::DCTRL, "hacause", hacause), "Failed to read DCTRL.hacause field for warp " + std::to_string(wid));
            }
            warp_status[wid] = {wid, active, halted, pc, hacause};
        }
    }
    
    // Restore original selection
    if (saved_wid >= 0 && saved_tid >= 0) {
        CHECK_ERR(select_warp_thread(saved_wid, saved_tid), "Failed to restore original warp/thread selection");
    }
    return RCODE_OK;
}

int Backend::get_warp_summary(bool *allhalted, bool *anyhalted, bool *allrunning, bool *anyrunning, bool *allunavail, bool *anyunavail) {
    uint32_t dctrl = 0;
    CHECK_ERR(dmreg_rd(DMReg_t::DCTRL, dctrl), "Failed to read DCTRL register");
    bool bit_allhalted = extract_dmreg_field(DMReg_t::DCTRL, "allhalted", dctrl) ? true : false;
    bool bit_anyhalted = extract_dmreg_field(DMReg_t::DCTRL, "anyhalted", dctrl) ? true : false;
    bool bit_allrunning = extract_dmreg_field(DMReg_t::DCTRL, "allrunning", dctrl) ? true : false;
    bool bit_anyrunning = extract_dmreg_field(DMReg_t::DCTRL, "anyrunning", dctrl) ? true : false;
    bool bit_allunavail = extract_dmreg_field(DMReg_t::DCTRL, "allunavail", dctrl) ? true : false;
    bool bit_anyunavail = extract_dmreg_field(DMReg_t::DCTRL, "anyunavail", dctrl) ? true : false;

    // return values
    if(allhalted) {
        *allhalted = bit_allhalted;
    }
    if(anyhalted) {
        *anyhalted = bit_anyhalted;
    }
    if(allrunning) {
        *allrunning = bit_allrunning;
    }
    if(anyrunning) {
        *anyrunning = bit_anyrunning;
    }
    if(allunavail) {
        *allunavail = bit_allunavail;
    }
    if(anyunavail) {
        *anyunavail = bit_anyunavail;
    }
    return RCODE_OK;
}

int Backend::get_warp_state(int g_wid, bool &halted) {
    if (g_wid < 0 || g_wid >= static_cast<int>(state_.platinfo.num_total_warps)) {
        log_->error("Invalid global warp ID " + std::to_string(g_wid));
        return RCODE_INVALID_ARG;
    }

    int win_idx = g_wid / 32;
    CHECK_ERR(dmreg_wrfield(DMReg_t::DSELECT, "winsel", win_idx), "Failed to write DSELECT.winsel field");

    uint32_t wstatus = 0;
    CHECK_ERR(dmreg_rd(DMReg_t::WSTATUS, wstatus), "Failed to read WSTATUS register");
    int bit_pos = g_wid % 32;

    halted = ((wstatus >> bit_pos) & 0x1) ? true : false;
    return RCODE_OK;
}


int Backend::get_warp_pc(uint32_t &pc) {
    CHECK_SELECTED();
    CHECK_ERR(dmreg_rd(DMReg_t::DPC, pc), "Failed to read DPC register");
    log_->debug(strfmt("Rd PC => 0x%08X", pc));
    return RCODE_OK;
}

int Backend::set_warp_pc(const uint32_t pc) {
    CHECK_SELECTED();
    CHECK_ERR(dmreg_wr(DMReg_t::DPC, pc), "Failed to write DPC register");
    log_->debug(strfmt("Wr PC <= 0x%08X", pc));
    return RCODE_OK;
}


//----- Warp Control Methods ---------------------------------------------------

int Backend::halt_warps(const std::vector<int> &wids) {
    log_->info("Halting warps: " + vecjoin<int>(wids));

    // Select the specified warps
    CHECK_ERR(select_warps(wids), "Failed to select warps for halting");

    // Send halt request
    CHECK_ERR(dmreg_wrfield(DMReg_t::DCTRL, "haltreq", 1), "Failed to send halt request");
       
    bool ok = true;
    for (int wid : wids) {
        bool warp_is_halted = false;
        CHECK_ERR(get_warp_state(wid, warp_is_halted), "Failed to get warp state after halt request");
        if (!warp_is_halted) {
            log_->warn("Warp " + std::to_string(wid) + " not halted after halt request");
            ok = false;
            continue;
        }
    }
    if (!ok) {
        log_->warn("Some warps failed to halt");
        return RCODE_ERROR;
    }

    log_->debug("Successfully halted warps: " + vecjoin<int>(wids, ", "));
    return RCODE_OK;
}

int Backend::halt_warps() {
    log_->info("Halting all warps");
    
    // Select all warps
    CHECK_ERR(select_warps(true), "Failed to select all warps for halting");
    
    // Send halt request
    CHECK_ERR(dmreg_wrfield(DMReg_t::DCTRL, "haltreq", 1), "Failed to send halt request");

    // Poll until all warps halted
    uint32_t allhalted;
    CHECK_ERR(dmreg_pollfield(DMReg_t::DCTRL, "allhalted", 1, &allhalted), "Failed to poll halt status");
    if (allhalted == 0) {
        log_->warn("Not all warps halted after halt request");
    }

    log_->debug("Successfully halted all warps");
    return RCODE_OK;
}

int Backend::resume_warps(const std::vector<int> &wids) {
    log_->info("Resuming warps: " + vecjoin<int>(wids));
    
    // Select the specified warps
    CHECK_ERR(select_warps(wids), "Failed to select warps for resuming");

    // Send resume request
    CHECK_ERR(dmreg_wrfield(DMReg_t::DCTRL, "resumereq", 1), "Failed to send resume request");

    // Check if given warps are running
    std::map<int, WarpStatus_t> warp_status;
    CHECK_ERR(get_warp_status(warp_status, false), "Failed to get warp status after resume request");     // also updates the halted_cache

    bool ok = true;
    for (int wid : wids) {
        bool warp_is_halted = false;
        CHECK_ERR(get_warp_state(wid, warp_is_halted), "Failed to get warp state after resume request");
        if (warp_is_halted) {
            log_->warn("Warp " + std::to_string(wid) + " not resumed after resume request");
            ok = false;
        }
    }
    if (!ok) {
        log_->warn("Some warps failed to resume");
        return RCODE_ERROR;
    }
    log_->debug("Successfully resumed warps: " + vecjoin<int>(wids, ", "));
    return RCODE_OK;
}

int Backend::resume_warps() {
    log_->info("Resuming all warps");
    
    // Select all warps
    CHECK_ERR(select_warps(true), "Failed to select all warps for resuming");

    // Send resume request
    CHECK_ERR(dmreg_wrfield(DMReg_t::DCTRL, "resumereq", 1), "Failed to send resume request");

    // Poll until all warps running
    uint32_t allrunning;
    CHECK_ERR(dmreg_pollfield(DMReg_t::DCTRL, "allrunning", 1, &allrunning), "Failed to poll resume status");
    
    log_->debug("Successfully resumed all warps");
    return RCODE_OK;
}

int Backend::step_warp() {
    CHECK_SELECTED();
    bool all_halted = false;
    CHECK_ERR(get_warp_summary(&all_halted), "Failed to get warp summary before stepping");
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
    state_.selected_warp_pc = warp_pc;
    
    return RCODE_OK;
}

int Backend::get_halt_cause(uint32_t &hacause) {
    CHECK_SELECTED();
    CHECK_ERR(dmreg_rdfield(DMReg_t::DCTRL, "cause", hacause), "Failed to read HACAUSE register");
    log_->debug(strfmt("Rd HACAUSE => 0x%08X", hacause));
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

int Backend::inject_instruction(const std::string &asm_instr) {
    // Assemble instruction
    auto instrs = rv_asm({asm_instr});
    if (instrs.size() != 1) {
        log_->error("Failed to assemble instruction: " + asm_instr);
        return RCODE_ERROR;
    }
    // Inject instruction
    CHECK_ERR(inject_instruction(instrs[0]), "Failed to inject instruction: " + asm_instr);
    return RCODE_OK;
}


// ----- Platform Query/Update Methods -----------------------------------------

int Backend::read_gpr(const uint32_t regnum, uint32_t &value) {
    // move arch reg to dscratch: REG[i] -csrw-> dscratch
    CHECK_ERR(inject_instruction(strfmt("csrw %d, x%d", RV_CSR_VX_DSCRATCH, regnum)), "Failed to move arch reg to dscratch");
    // read reg value from dscratch
    CHECK_ERR(dmreg_rd(DMReg_t::DSCRATCH, value), "Failed to obtain GPR value from DSCRATCH");
    log_->debug(strfmt("Rd GPR[x%d] => 0x%08X", regnum, value));
    return RCODE_OK;
}

int Backend::write_gpr(const uint32_t regnum, const uint32_t value) {
    // move value to dscratch: dbg(value) --> dscratch
    CHECK_ERR(dmreg_wr(DMReg_t::DSCRATCH, value), "Failed to write DSCRATCH register");
    // move dscratch to arch reg: dscratch -csrr-> REG[i]
    CHECK_ERR(inject_instruction(strfmt("csrr x%d, %d", regnum, RV_CSR_VX_DSCRATCH)), "Failed to move dscratch to GPR");
    log_->debug(strfmt("Wr GPR[x%d] <= 0x%08X", regnum, value));
    return RCODE_OK;
}

// Get the CSR register value
int Backend::read_csr(const uint32_t regaddr, uint32_t &value) {
    // save t0: t0 -csrw-> dscratch --> dbg (t0_val)
    uint32_t t0_val = 0;
    CHECK_ERR(inject_instruction(strfmt("csrw %d, t0", RV_CSR_VX_DSCRATCH)), "Failed to save t0 to DSCRATCH");          // TODO: if any subsequent inject fails, fn returns without restoring t0 --> need RAII guard
    CHECK_ERR(dmreg_rd(DMReg_t::DSCRATCH, t0_val), "Failed to obtain t0 value from DSCRATCH");
    // move csr to dscratch through t0: csr -csrr-> t0 ; t0 -csrw-> dscratch
    CHECK_ERR(inject_instruction(strfmt("csrr t0, %d", regaddr)), "Failed to read CSR into t0");
    CHECK_ERR(inject_instruction(strfmt("csrw %d, t0", RV_CSR_VX_DSCRATCH)), "Failed to write t0 to DSCRATCH");
    // read csr value from dscratch: dscratch -dbg-> value
    CHECK_ERR(dmreg_rd(DMReg_t::DSCRATCH, value), "Failed to obtain CSR value from DSCRATCH");
    // restore t0: dbg(t0_val) --> dscratch -csrr-> t0
    CHECK_ERR(dmreg_wr(DMReg_t::DSCRATCH, t0_val), "Failed to restore t0 value to DSCRATCH");
    CHECK_ERR(inject_instruction(strfmt("csrr t0, %d", RV_CSR_VX_DSCRATCH)), "Failed to restore t0 from DSCRATCH");
    log_->debug(strfmt("Rd CSR[0x%03X] => 0x%08X", regaddr, value));
    return RCODE_OK;
}

// Set the CSR register value
int Backend::write_csr(const uint32_t regaddr, const uint32_t value) {
    // save t0: t0 -csrw-> dscratch --> dbg (t0_val)
    uint32_t t0_val = 0;
    CHECK_ERR(inject_instruction(strfmt("csrw %d, t0", RV_CSR_VX_DSCRATCH)), "Failed to save t0 to DSCRATCH");          // TODO: if any subsequent inject fails, fn returns without restoring t0 --> need RAII guard
    CHECK_ERR(dmreg_rd(DMReg_t::DSCRATCH, t0_val), "Failed to obtain t0 value from DSCRATCH");
    // move value to dscratch: dbg(value) --> dscratch
    CHECK_ERR(dmreg_wr(DMReg_t::DSCRATCH, value), "Failed to write value to DSCRATCH");
    // move dscratch to csr through t0: dscratch -csrr-> t0 ; t0 -csrw-> csr
    CHECK_ERR(inject_instruction(strfmt("csrr t0, %d", RV_CSR_VX_DSCRATCH)), "Failed to read DSCRATCH into t0");
    CHECK_ERR(inject_instruction(strfmt("csrw %d, t0", regaddr)), "Failed to write t0 to CSR");
    // restore t0: dbg(t0_val) --> dscratch -csrr-> t0
    CHECK_ERR(dmreg_wr(DMReg_t::DSCRATCH, t0_val), "Failed to restore t0 value to DSCRATCH");
    CHECK_ERR(inject_instruction(strfmt("csrr t0, %d", RV_CSR_VX_DSCRATCH)), "Failed to restore t0 from DSCRATCH");
    log_->debug(strfmt("Wr CSR[0x%03X] <= 0x%08X", regaddr, value));
    return RCODE_OK;
}

int Backend::read_reg(const std::string &reg_name, uint32_t &value) {
    auto regtype = rvreg_gettype(reg_name);
    if (regtype == RVRegType_t::GPR) {
        uint32_t regnum = rvgpr_name2num(reg_name);
        CHECK_ERR(read_gpr(regnum, value), "Failed to read GPR " + reg_name);
        return RCODE_OK;
    }
    else if (regtype == RVRegType_t::CSR) {
        uint32_t csraddr = rvcsr_name2addr(reg_name);
        CHECK_ERR(read_csr(csraddr, value), "Failed to read CSR " + reg_name);
        return RCODE_OK;
    }
    else if (reg_name == "pc") {
        CHECK_ERR(get_warp_pc(value), "Failed to read PC register");
        return RCODE_OK;
    }
    else {
        log_->error("Unknown register name: " + reg_name);
        return RCODE_INVALID_ARG;
    }
    return RCODE_OK;
}

// Write processor register
int Backend::write_reg(const std::string &reg_name, uint32_t value) {
    auto regtype = rvreg_gettype(reg_name);
    if (regtype == RVRegType_t::GPR) {
        uint32_t regnum = rvgpr_name2num(reg_name);
        CHECK_ERR(write_gpr(regnum, value), "Failed to write GPR " + reg_name);
        return RCODE_OK;
    }
    else if (regtype == RVRegType_t::CSR) {
        uint32_t csraddr = rvcsr_name2addr(reg_name);
        CHECK_ERR(write_csr(csraddr, value), "Failed to write CSR " + reg_name);
        return RCODE_OK;
    }
    else if (reg_name == "pc") {
        CHECK_ERR(set_warp_pc(value), "Failed to write PC register");
        return RCODE_OK;
    }
    else {
        log_->error("Unknown register name: " + reg_name);
        return RCODE_INVALID_ARG;
    }
    return RCODE_OK;
}

int Backend::read_regs(const std::vector<std::string> &reg_names, std::vector<uint32_t> &values) {
    values.clear();
    for (const auto &reg_name : reg_names) {            // TODO: Optimize multiple register reads
        uint32_t value = 0;
        CHECK_ERR(read_reg(reg_name, value), "Failed to read register " + reg_name);
        values.push_back(value);
    }
    return RCODE_OK;
}

int Backend::write_regs(const std::vector<std::string> &reg_names, const std::vector<uint32_t> &values) {
    if (reg_names.size() != values.size()) {
        log_->error("Number of register names and values do not match");
        return RCODE_INVALID_ARG;
    }
    for (size_t i = 0; i < reg_names.size(); ++i) {     // TODO: Optimize multiple register writes
        CHECK_ERR(write_reg(reg_names[i], values[i]), "Failed to write register " + reg_names[i]);
    }
    return RCODE_OK;
}

// Read memory
int Backend::read_mem(const uint32_t addr, const uint32_t nbytes, std::vector<uint8_t> &data) {
    if(nbytes == 0)
        return RCODE_OK;

    data.clear();
    
    // Compute word aligned address
    uint32_t start_addr = addr & ~0x3;
    uint32_t end_addr = (addr + nbytes + 3) & ~0x3;   // Align to next word
    uint32_t size_in_bytes = end_addr - start_addr;

    data.resize(size_in_bytes);     // allocate buffer

    // Save t0 & t1: t0/t1 -csrw-> dscratch --> dbg (t0_val/t1_val)
    uint32_t t0_val = 0, t1_val = 0;
    CHECK_ERR(inject_instruction(strfmt("csrw %d, t0", RV_CSR_VX_DSCRATCH)), "Failed to save t0 to DSCRATCH");          // TODO: if any subsequent inject fails, fn returns without restoring t0,t1 --> need RAII guard
    CHECK_ERR(dmreg_rd(DMReg_t::DSCRATCH, t0_val), "Failed to obtain t0 value from DSCRATCH");
    CHECK_ERR(inject_instruction(strfmt("csrw %d, t1", RV_CSR_VX_DSCRATCH)), "Failed to save t1 to DSCRATCH");
    CHECK_ERR(dmreg_rd(DMReg_t::DSCRATCH, t1_val), "Failed to obtain t1 value from DSCRATCH");

    // Put start address in t0: dbg(start_addr) --> dscratch -csrr-> t0
    CHECK_ERR(dmreg_wr(DMReg_t::DSCRATCH, start_addr), "Failed to write start address to DSCRATCH");
    CHECK_ERR(inject_instruction(strfmt("csrr t0, %d", RV_CSR_VX_DSCRATCH)), "Failed to load start address into t0");

    // Read words

    // pre format and compile loop instructions for speed
    const uint32_t lw_t1 = rv_asm({"lw t1, 0(t0)"}).at(0);
    const uint32_t csrw_dscratch_t1 = rv_asm({strfmt("csrw %d, t1", RV_CSR_VX_DSCRATCH)}).at(0);
    const uint32_t addi_t0_4 = rv_asm({"addi t0, t0, 4"}).at(0);

    for (size_t i = 0; i < size_in_bytes; i += 4) {
        uint32_t rword;
        // Load word from memory to t1
        CHECK_ERR(inject_instruction(lw_t1), "Failed to load word from memory");
        // Move t1 to dscratch for reading
        CHECK_ERR(inject_instruction(csrw_dscratch_t1), "Failed to write t1 to DSCRATCH");
        CHECK_ERR(dmreg_rd(DMReg_t::DSCRATCH, rword), "Failed to obtain word from DSCRATCH");
        // Increment address in t0
        CHECK_ERR(inject_instruction(addi_t0_4), "Failed to increment address in t0");
        // write data to buffer
        std::memcpy(&data[i], &rword, sizeof(rword));
    }

    // Restore t0 & t1
    CHECK_ERR(dmreg_wr(DMReg_t::DSCRATCH, t0_val), "Failed to restore t0 value to DSCRATCH");
    CHECK_ERR(inject_instruction(strfmt("csrr t0, %d", RV_CSR_VX_DSCRATCH)), "Failed to restore t0 from DSCRATCH");
    CHECK_ERR(dmreg_wr(DMReg_t::DSCRATCH, t1_val), "Failed to restore t1 value to DSCRATCH");
    CHECK_ERR(inject_instruction(strfmt("csrr t1, %d", RV_CSR_VX_DSCRATCH)), "Failed to restore t1 from DSCRATCH");


    // trim extra bytes
    size_t head_offset = addr - start_addr;
    if (head_offset || size_in_bytes != nbytes) {
        // Adjust for unaligned start address and requested size
        if (head_offset) data.erase(data.begin(), data.begin() + head_offset);
        // Trim to requested size
        if (data.size() > nbytes) data.resize(nbytes);
    }
    return RCODE_OK;
}

// Write memory
int Backend::write_mem(const uint32_t addr, const std::vector<uint8_t> &data) {
    size_t nbytes = data.size();   
    if (nbytes == 0)
        return RCODE_OK;

    // Compute word aligned address
    uint32_t end_addr   = (addr + nbytes);
    
    // --- Save t0 & t1 ---
    uint32_t t0_val = 0, t1_val = 0;
    CHECK_ERR(inject_instruction(strfmt("csrw %d, t0", RV_CSR_VX_DSCRATCH)), "Failed to save t0");
    CHECK_ERR(dmreg_rd(DMReg_t::DSCRATCH, t0_val), "Failed to read t0");
    CHECK_ERR(inject_instruction(strfmt("csrw %d, t1", RV_CSR_VX_DSCRATCH)), "Failed to save t1");
    CHECK_ERR(dmreg_rd(DMReg_t::DSCRATCH, t1_val), "Failed to read t1");

    // --- Pre-assemble common instructions ---
    const uint32_t csrr_t0_dscratch = rv_asm({strfmt("csrr t0, %d", RV_CSR_VX_DSCRATCH)}).at(0);
    const uint32_t csrr_t1_dscratch = rv_asm({strfmt("csrr t1, %d", RV_CSR_VX_DSCRATCH)}).at(0);
    const uint32_t csrw_dscratch_t1 = rv_asm({strfmt("csrw %d, t1", RV_CSR_VX_DSCRATCH)}).at(0);
    const uint32_t lw_t1_t0         = rv_asm({"lw t1, 0(t0)"}).at(0);
    const uint32_t sw_t1_t0         = rv_asm({"sw t1, 0(t0)"}).at(0);
    const uint32_t addi_t0_4        = rv_asm({"addi t0, t0, 4"}).at(0);

    uint32_t cur = addr;    // running address
    size_t idx = 0;

    // --- Leading partial word ---
    if (cur % 4 != 0) {
        uint32_t curr_base_addr = cur & ~0x3;
        // Synchronize t0 with current address
        CHECK_ERR(dmreg_wr(DMReg_t::DSCRATCH, curr_base_addr), "Failed to write base addr");
        CHECK_ERR(inject_instruction(csrr_t0_dscratch), "Failed to read t0 from DSCRATCH");

        // Read original value
        WordBytes_t value;
        CHECK_ERR(inject_instruction(lw_t1_t0), "Failed to load original word into t1");
        CHECK_ERR(inject_instruction(csrw_dscratch_t1), "Failed to save t1");
        CHECK_ERR(dmreg_rd(DMReg_t::DSCRATCH, value.word), "Failed to read orig word");

        // Patch original word
        size_t head_offset = cur % 4;
        size_t take = std::min<size_t>(4 - head_offset, data.size() - idx);
        for (unsigned i = head_offset; i < head_offset + take; ++i)
            value.bytes[i] = data[idx++];
        cur += take;

        // Write back patched word
        CHECK_ERR(dmreg_wr(DMReg_t::DSCRATCH, value.word), "Failed to write patched word");
        CHECK_ERR(inject_instruction(csrr_t1_dscratch), "Failed to read t1 from DSCRATCH");
        CHECK_ERR(inject_instruction(sw_t1_t0), "Failed to store word into memory");
    }

    // --- Middle full words ---
    if (end_addr - cur >= 4) {  // at least one full word to write
        // Synchronize t0 with current address
        CHECK_ERR(dmreg_wr(DMReg_t::DSCRATCH, cur), "Failed to write current addr");
        CHECK_ERR(inject_instruction(csrr_t0_dscratch), "Failed to read t0 from DSCRATCH");

        while ((end_addr - cur) >= 4) {
            WordBytes_t value;
            std::memcpy(value.bytes, &data[idx], 4);
            CHECK_ERR(dmreg_wr(DMReg_t::DSCRATCH, value.word), "Failed to write word to DSCRATCH");
            CHECK_ERR(inject_instruction(csrr_t1_dscratch), "Failed to read t1 from DSCRATCH");
            CHECK_ERR(inject_instruction(sw_t1_t0), "Failed to store word into memory");
            CHECK_ERR(inject_instruction(addi_t0_4), "Failed to increment t0");
            cur += 4;
            idx += 4;
        }
    }
        
    // --- Trailing partial word ---
    if (cur < end_addr) {
        // synchronize t0 with current address
        uint32_t base = cur & ~0x3;
        CHECK_ERR(dmreg_wr(DMReg_t::DSCRATCH, base), "Failed to write current addr");
        CHECK_ERR(inject_instruction(csrr_t0_dscratch), "Failed to read t0 from DSCRATCH");

        // Read original word
        WordBytes_t value;
        CHECK_ERR(inject_instruction(lw_t1_t0), "Failed to load original word into t1");
        CHECK_ERR(inject_instruction(csrw_dscratch_t1), "Failed to save t1");
        CHECK_ERR(dmreg_rd(DMReg_t::DSCRATCH, value.word), "Failed to read orig word");

        // Patch original word
        size_t take = end_addr - cur;
        for (unsigned i = 0; i < take; ++i)
            value.bytes[i] = data[idx++];
        cur += take;

        // Write back patched word
        CHECK_ERR(dmreg_wr(DMReg_t::DSCRATCH, value.word), "Failed to write patched word to DSCRATCH");
        CHECK_ERR(inject_instruction(csrr_t1_dscratch), "Failed to read t1 from DSCRATCH");
        CHECK_ERR(inject_instruction(sw_t1_t0), "Failed to store word into memory");
    }

    // --- Restore t0 & t1 ---
    CHECK_ERR(dmreg_wr(DMReg_t::DSCRATCH, t0_val), "Failed to restore t0 value to DSCRATCH");
    CHECK_ERR(inject_instruction(strfmt("csrr t0, %d", RV_CSR_VX_DSCRATCH)), "Failed to restore t0 from DSCRATCH");
    CHECK_ERR(dmreg_wr(DMReg_t::DSCRATCH, t1_val), "Failed to restore t1 value to DSCRATCH");
    CHECK_ERR(inject_instruction(strfmt("csrr t1, %d", RV_CSR_VX_DSCRATCH)), "Failed to restore t1 from DSCRATCH");

    return RCODE_OK;
}

// ----- Breakpoint Management -------------------------------------------------

int Backend::set_breakpoint(uint32_t addr) {
    // Check if breakpoint already exists
    auto it = breakpoints_.find(addr);
    if (it != breakpoints_.end() && it->second.enabled) {
        log_->warn(strfmt("Breakpoint already exists at 0x%08X", addr));
        return RCODE_OK;
    }

    // Get instruction at address
    WordBytes_t instr;
    std::vector<uint8_t> instr_data;
    CHECK_ERR(read_mem(addr, 4, instr_data), "Failed to read instruction for breakpoint");
    std::copy(instr_data.begin(), instr_data.begin() + 4, instr.bytes);

    // Write ebreak instruction at the address
    WordBytes_t ebreak_instr;
    ebreak_instr.word = rv_asm({"ebreak"}).at(0);
    std::vector<uint8_t> ebreak_data(ebreak_instr.bytes, ebreak_instr.bytes + 4);
    CHECK_ERR(write_mem(addr, ebreak_data), "Failed to write ebreak instruction for breakpoint");
    
    // Add to breakpoint table
    BreakPointInfo_t bpinfo = {
        .enabled = true,
        .addr = addr,
        .replaced_instr = instr.word,
        .hit_count = 0
    };
    breakpoints_[addr] = bpinfo;    // insert new breakpoint

    log_->info(strfmt("Breakpoint set at 0x%08X", addr));
    return RCODE_OK;
}

int Backend::remove_breakpoint(uint32_t addr) {
    // Find breakpoint
    auto it = breakpoints_.find(addr);
    if (it == breakpoints_.end() || !it->second.enabled) {
        log_->warn(strfmt("No breakpoint found at 0x%08X", addr));
        return RCODE_OK;
    }

    // Restore original instruction
    WordBytes_t orig_instr;
    orig_instr.word = it->second.replaced_instr;
    std::vector<uint8_t> orig_data(orig_instr.bytes, orig_instr.bytes + 4);
    CHECK_ERR(write_mem(addr, orig_data), "Failed to restore original instruction for breakpoint");

    // Remove from breakpoint table
    breakpoints_.erase(it);

    log_->info(strfmt("Breakpoint removed at 0x%08X", addr));
    return RCODE_OK;
}

std::unordered_map<uint32_t, BreakPointInfo_t> Backend::get_breakpoints() const {
    return breakpoints_;
}

int Backend::any_breakpoints(bool &anybps) const {
    for (const auto& [addr, bpinfo] : breakpoints_) {
        if (bpinfo.enabled) {
            anybps = true;
            return RCODE_OK;
        }
    }
    anybps = false;
    return RCODE_OK;
}

int Backend::continue_until_breakpoint() {
    log_->info("Continuing execution until breakpoint is hit");

    // Resume all warps
    CHECK_ERR(resume_warps(), "Failed to resume warps");

    log_->warn("continue until breakpoint not supported yet");
    return RCODE_OK;

}

//==============================================================================
// Helpers
//==============================================================================

void Backend::_print_platform_info() const {
    std::string info;
    info += strfmt("  Platform ID   : 0x%08X\n", state_.platinfo.platform_id);
    info += strfmt("  Platform Name : %s\n", state_.platinfo.platform_name.c_str());
    info += strfmt("  ISA           : %s (%s)\n", rv_isa_string(state_.platinfo.misa).c_str(), rv_isa_string(state_.platinfo.misa, true).c_str());
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

//==============================================================================
// Low-level DM register access
//==============================================================================

int Backend::dmreg_rd(const DMReg_t &reg, uint32_t &value) {
    CHECK_TRANSPORT();
    const auto& rinfo = get_dmreg(reg);
    CHECK_ERR(transport_->read_reg(rinfo.addr, value), "Failed to read DM register " + std::string(rinfo.name));
    log_->debug(strfmt("Rd DM.%s(0x%02X) => 0x%08X", rinfo.name.data(), rinfo.addr, value));
    return RCODE_OK;
}

int Backend::dmreg_wr(const DMReg_t &reg, const uint32_t &value) {
    CHECK_TRANSPORT();
    const auto& rinfo = get_dmreg(reg);
    CHECK_ERR(transport_->write_reg(rinfo.addr, value), "Failed to write DM register " + std::string(rinfo.name));
    log_->debug(strfmt("Wr DM.%s(0x%02X) <= 0x%08X", rinfo.name.data(), rinfo.addr, value));
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
        log_->debug(strfmt("Rd DM.%s(0x%02X).%s => 0x%X", rinfo.name.data(), rinfo.addr, finfo->name.data(), value));
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

        log_->debug(strfmt("Wr DM.%s(0x%02X).%s <= 0x%X (NewRegVal: 0x%08X, OldRegVal: 0x%08X)", 
                    rinfo.name.data(), rinfo.addr, finfo->name.data(), value, new_reg_value, curr_reg_value));
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
    
    // Use default values if not provided
    if (max_retries == -1) max_retries = poll_retries_;
    if (delay_ms == -1) delay_ms = poll_delay_ms_;
    
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
            log_->debug(strfmt("Poll DM.%s(0x%02X).%s => 0x%X == 0x%X (RegVal: 0x%08X)", 
                        rinfo.name.data(), rinfo.addr, finfo->name.data(), value, exp_value, reg_value));
            
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
