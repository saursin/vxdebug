#include "backend.h"
#include "transport.h"
#include "logger.h"
#include "util.h"
#include <unistd.h>

#ifndef VORTEX_PLATFORMID
    #define VORTEX_PLATFORMID 0x1
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

//==============================================================================
// Transport management
//==============================================================================

void Backend::setup_transport(const std::string &type) {
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
    }
}

void Backend::connect_transport(const std::map<std::string, std::string> &args) {
    if (!transport_) {
        log_->warn("Transport not set up, cannot connect.");
        return;
    }
    if (transport_type_ == "tcp") {
        int rc = transport_->connect(args);
        if (rc != RCODE_OK) {
            log_->error("Failed to connect TCP transport.");
            return;
        }
        log_->info("Transport connected!");
    } else {
        log_->error("Transport type not supported for connection: " + transport_type_);
    }
}

void Backend::disconnect_transport() {
    if (!transport_) {
        log_->warn("Transport not set up, nothing to disconnect.");
        return;
    }
    int rc = transport_->disconnect();
    if (rc != 0) {
        log_->error("Failed to disconnect transport.");
    }
}

bool Backend::is_transport_connected() const {
    if (!transport_) {
        log_->warn("Transport not set up, cannot retrive status");
        return false;
    }
    return transport_->is_connected();
}


//==============================================================================
// Initialization
//==============================================================================

void Backend::initialize() {
    if (!_check_transport_connected()) {
        log_->error("Cannot initialize backend: Transport not connected.");
        return;
    }
    log_->info("Initializing backend...");

    // Try to Wake DM
    wake_dm();

    // Get platform info
    get_platform_info();

    log_->info("Backend initialized.");

    // Print platform info
    _print_platform_info();
}


//==============================================================================
// API Methods
//==============================================================================
void Backend::wake_dm() {  
    int rc;

    // Check if ndmreset is set
    uint32_t ndmreset = 0;
    rc = dmreg_rdfield(DMReg_t::DCTRL, "ndmreset", ndmreset);
    if (rc != 0) {
        log_->error("Failed to read DCTRL.ndmreset field (rc=" + std::to_string(rc)+")");
        return;
    }
    if (ndmreset) {
        // Wait for ndmreset to low
        log_->debug("Waiting for DCTRL.ndmreset to clear...");
        rc = dmreg_pollfield(DMReg_t::DCTRL, "ndmreset", 0, &ndmreset, wake_dm_retries_, wake_dm_delay_ms_);
        if (rc != 0) {
            log_->error("Failed to poll DCTRL.ndmreset field (rc=" + std::to_string(rc)+")");
            return;
        }
    }

    // Check if dm is active
    uint32_t dmactive = 0;
    rc = dmreg_rdfield(DMReg_t::DCTRL, "dmactive", dmactive);
    if (rc != 0) {
        log_->error("Failed to read DCTRL.dmactive field (rc=" + std::to_string(rc)+")");
        return;
    }
    if (!dmactive) {
        // DM is not active, need to wake it up
        log_->debug("DM not active, Waking up DM by setting DCTRL.dmactive...");
        do {
            rc = dmreg_wrfield(DMReg_t::DCTRL, "dmactive", 1);
            if (rc != 0) {
                log_->error("Failed to write DCTRL.dmactive field (rc=" + std::to_string(rc)+")");
                return;
            }
            rc = dmreg_pollfield(DMReg_t::DCTRL, "dmactive", 1, &dmactive, wake_dm_retries_, wake_dm_delay_ms_);
            if (rc != 0) {
                log_->warn("Failed to poll DCTRL.dmactive field (rc=" + std::to_string(rc) + "), retrying...");
                return;
            }
        } while (dmactive == 0);
    }
    log_->debug("DM is awake!");
    return;
}

void Backend::reset(bool halt_warps) {
    int rc;
    if (!halt_warps) {
        // TODO: select all warps
    }

    // Issue system reset via DCTRL.ndmreset
    log_->info("Issuing system reset...");
    rc = dmreg_wrfield(DMReg_t::DCTRL, "ndmreset", 1);
    if (rc != 0) {
        log_->error("Failed to set DCTRL.ndmreset field (rc=" + std::to_string(rc) + ")");
        return;
    }

    // Wait for reset to complete (ndmreset to clear)
    uint32_t ndmreset = 1;
    rc = dmreg_pollfield(DMReg_t::DCTRL, "ndmreset", 0, &ndmreset, wake_dm_retries_, wake_dm_delay_ms_);
    if (rc != 0) {
        log_->error("Failed to poll DCTRL.ndmreset field after reset (rc=" + std::to_string(rc) + ")");
        return;
    }

    if(halt_warps) {
        // TODO: check if all warps are halted
    }


    log_->info("System reset complete.");
    return;
}

void Backend::get_platform_info() {
    int rc;
    uint32_t platform = 0;
    rc = dmreg_rd(DMReg_t::PLATFORM, platform);
    if (rc != 0) {
        log_->error("Failed to read PLATFORM register (rc=" + std::to_string(rc) + ")");
        return;
    }

    state_.platinfo.platform_id     = extract_dmreg_field(DMReg_t::PLATFORM, "platformid", platform);
    state_.platinfo.platform_name   = state_.platinfo.platform_id == VORTEX_PLATFORMID ? "Vortex" : "Unknown";
    state_.platinfo.num_clusters    = extract_dmreg_field(DMReg_t::PLATFORM, "numclusters", platform);
    state_.platinfo.num_cores       = extract_dmreg_field(DMReg_t::PLATFORM, "numcores", platform);
    state_.platinfo.num_warps       = extract_dmreg_field(DMReg_t::PLATFORM, "numwarps", platform);
    state_.platinfo.num_threads     = extract_dmreg_field(DMReg_t::PLATFORM, "numthreads", platform);
    state_.platinfo.num_total_cores = state_.platinfo.num_clusters * state_.platinfo.num_cores;
    state_.platinfo.num_total_warps = state_.platinfo.num_total_cores * state_.platinfo.num_warps;
    state_.platinfo.num_total_threads = state_.platinfo.num_total_warps * state_.platinfo.num_threads;
    return;
}

void Backend::select_warps(const std::vector<int> &wids) {
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
        int rc = 0;
        rc = dmreg_wrfield(DMReg_t::DSELECT, "winsel", i);
        if (rc != 0) {
            log_->error("Failed to write DSELECT.winsel field (rc=" + std::to_string(rc) + ")");
            return;
        }
        rc = dmreg_wrfield(DMReg_t::WMASK, "mask", win_masks[i]);
        if (rc != 0) {
            log_->error("Failed to write WMASK.mask field (rc=" + std::to_string(rc) + ")");
            return;
        }
    }
    log_->info("Selected " + std::to_string(wids.size()) + " warps.");  
    return;
}

void Backend::select_warps(bool all) {
    size_t num_win = (state_.platinfo.num_total_warps + 31) / 32;
    for (size_t i = 0; i < num_win; ++i) {
        int rc = 0;
        rc = dmreg_wrfield(DMReg_t::DSELECT, "winsel", i);
        if (rc != 0) {
            log_->error("Failed to write DSELECT.winsel field (rc=" + std::to_string(rc) + ")");
            return;
        }
        rc = dmreg_wrfield(DMReg_t::WMASK, "mask", all ? 0xFFFFFFFF : 0x00000000);
        if (rc != 0) {
            log_->error("Failed to write WMASK.mask field (rc=" + std::to_string(rc) + ")");
            return;
        }
    }
    log_->info(all ? "Selected all warps." : "Deselected all warps.");
    return;
}

void Backend::select_dbg_thread(int g_wid, int tid) {
    if (g_wid < 0 || g_wid >= static_cast<int>(state_.platinfo.num_total_warps)) {
        log_->error("Invalid global warp ID " + std::to_string(g_wid));
        return;
    }
    if (tid < 0 || tid >= static_cast<int>(state_.platinfo.num_threads)) {
        log_->error("Invalid thread ID " + std::to_string(tid));
        return;
    }
    int rc = 0;
    rc = dmreg_wrfield(DMReg_t::DSELECT, "warpsel", g_wid);
    if (rc != 0) {
        log_->error("Failed to write DSELECT.warpsel field (rc=" + std::to_string(rc) + ")");
        return;
    }
    rc = dmreg_wrfield(DMReg_t::DSELECT, "threadsel", tid);
    if (rc != 0) {
        log_->error("Failed to write DSELECT.threadsel field (rc=" + std::to_string(rc) + ")");
        return;
    }
    state_.selected_wid = g_wid;
    state_.selected_tid = tid;
    log_->info("Selected warp " + std::to_string(g_wid) + ", thread " + std::to_string(tid) + " for debugging.");
}



bool Backend::all_halted() {
    uint32_t v = 0;
    dmreg_rdfield(DMReg_t::DCTRL, "allhalted", v);
    return v == 1;
}

bool Backend::any_halted() {
    uint32_t v = 0;
    dmreg_rdfield(DMReg_t::DCTRL, "anyhalted", v);
    return v == 1;
}

bool Backend::all_running() {
    uint32_t v = 0;
    dmreg_rdfield(DMReg_t::DCTRL, "allrunning", v);
    return v == 1;
}

bool Backend::any_running() {
    uint32_t v = 0;
    dmreg_rdfield(DMReg_t::DCTRL, "anyrunning", v);
    return v == 1;
}


// =============================================================================
// Helpers
// =============================================================================

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

bool Backend::_check_transport_connected() const {
    if (!transport_) {
        log_->error("Transport not set up, cannot retrieve status");
        return false;
    }
    return transport_->is_connected();
}


// =============================================================================
// Low-level DM register access
// =============================================================================
int Backend::dmreg_rd(const DMReg_t &reg, uint32_t &value) {
    const auto& rinfo = get_dmreg(reg);
    int rc = transport_->read_reg(rinfo.addr, value);
    log_->debug("Rd DM[" + std::string(rinfo.name) + "] = 0x" + hex2str(value, 8));
    if (rc != 0) {
        log_->error("Failed to read DM register " + std::string(rinfo.name));
        return rc;
    }
    return RCODE_OK;
}

int Backend::dmreg_wr(const DMReg_t &reg, const uint32_t &value) {
    const auto& rinfo = get_dmreg(reg);
    int rc = transport_->write_reg(rinfo.addr, value);
    log_->debug("Wr DM[" + std::string(rinfo.name) + "] = 0x" + hex2str(value, 8));
    if (rc != 0) {
        log_->error("Failed to write DM register " + std::string(rinfo.name));
        return rc;
    }
    return RCODE_OK;
}

int Backend::dmreg_rdfield(const DMReg_t &reg, const std::string &fieldname, uint32_t &value) {
    const auto& rinfo = get_dmreg(reg);
    const FieldInfo_t* finfo = get_dmreg_field(reg, fieldname);
    if (!finfo) {
        log_->error("Invalid field name: " + fieldname);
        return RCODE_INVALID_ARG;
    }

    uint32_t reg_value = 0;
    int rc = transport_->read_reg(rinfo.addr, reg_value);

    uint32_t field_mask = finfo->mask();
    value = (reg_value & field_mask) >> finfo->lsb;
    log_->debug("Rd DM[" + std::string(rinfo.name) + "." + std::string(finfo->name) + "] = 0x" + hex2str(value));
    if (rc != 0) {
        log_->error("Failed to read DM register " + std::string(rinfo.name));
        return rc;
    }
    return RCODE_OK;
}

int Backend::dmreg_wrfield(const DMReg_t &reg, const std::string &fieldname, const uint32_t &value) {
    const auto& rinfo = get_dmreg(reg);
    const FieldInfo_t* finfo = get_dmreg_field(reg, fieldname);
    if (!finfo) {
        log_->error("Invalid field name: " + fieldname);
        return RCODE_INVALID_ARG;
    }

    // Read current register value
    uint32_t reg_value = 0;
    int rc = transport_->read_reg(rinfo.addr, reg_value);
    if (rc != 0) {
        log_->error("Failed to read DM register " + std::string(rinfo.name));
        return rc;
    }

    // Modify only the specific field
    uint32_t field_mask = finfo->mask();
    reg_value = (reg_value & ~field_mask) | ((value << finfo->lsb) & field_mask);
    
    // Write back the modified register
    rc = transport_->write_reg(rinfo.addr, reg_value);
    if (rc != 0) {
        log_->error("Failed to write DM register " + std::string(rinfo.name));
        return rc;
    }
    
    log_->debug("Wr DM[" + std::string(rinfo.name) + "." + std::string(finfo->name) + "] = 0x" + hex2str(value));
    return RCODE_OK;
}

int Backend::dmreg_pollfield(const DMReg_t &reg, const std::string &fieldname, const uint32_t &exp_value, uint32_t *final_value,
                        int max_retries, int delay_ms) {
    const auto& rinfo = get_dmreg(reg);
    const FieldInfo_t* finfo = get_dmreg_field(reg, fieldname);
    if (!finfo) {
        log_->error("Invalid field name: " + fieldname);
        return RCODE_INVALID_ARG;
    }

    uint32_t field_mask = finfo->mask();
    uint32_t value = 0;
    
    for (int attempt = 0; attempt < max_retries; ++attempt) {
        uint32_t reg_value = 0;
        int rc = transport_->read_reg(rinfo.addr, reg_value);
        if (rc != 0) {
            log_->error("Failed to read DM register " + std::string(rinfo.name) + " on attempt " + std::to_string(attempt + 1));
            return rc;
        }
        
        value = (reg_value & field_mask) >> finfo->lsb;
        log_->debug("Poll DM[" + std::string(rinfo.name) + "." + std::string(finfo->name) + "] = 0x" + hex2str(value) + 
                   " (attempt " + std::to_string(attempt + 1) + "/" + std::to_string(max_retries) + ")");
        
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
}
