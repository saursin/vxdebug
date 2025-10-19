#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

#include "dmdefs.h"

// Forward declarations
class Transport;
class Logger;


class Backend {
public:
    Backend();
    ~Backend();

    // Transport management
    void setup_transport(const std::string &type);
    void connect_transport(const std::map<std::string, std::string> &args);
    void disconnect_transport();
    bool is_transport_connected() const;

    // Initialization
    void initialize();

    //==========================================================================
    // API Methods
    //==========================================================================

    //----- DM Control -----------------------

    // Wakes up the Debug Module
    void wake_dm();

    // Resets the target system, optionally halting all warps after reset
    void reset(bool halt_warps=false);

    // Retrieves platform information from the target
    void get_platform_info();

    //----- Query Warp Status --------------
    // Returns a map of warp ID to (halted status, PC value)
    std::map<int, std::pair<bool, uint32_t>> get_warp_status(bool include_pc = true);
    
    //----- Warp Selection -----------------
    // Select specific warps by their warp IDs
    void select_warps(const std::vector<int> &wids);

    // Select all warps, or None if all=false
    void select_warps(bool all);

    // Select a specific global warp and thread for debugging
    void select_warp_thread(int g_wid, int tid);

    //----- Warp Control -----------------
    // Halt specific warps
    void halt_warps(const std::vector<int> &wids);
    
    // Halt all warps
    void halt_warps();
    
    // Resume specific warps
    void resume_warps(const std::vector<int> &wids);

    // Resume all warps
    void resume_warps();

    bool all_halted();
    bool any_halted();
    bool all_running();
    bool any_running();

    //----- State Getters -----------------
    int get_selected_warp_thread(int &wid, int &tid, bool force_fetch=false);


private:
    // Transport 
    Transport *transport_;
    std::string transport_type_;
    Logger *log_;

    // Current Debugger state
    struct State_t {
        int selected_wid = -1;
        int selected_tid = -1;

        struct PlatformInfo {
            uint32_t platform_id;
            std::string platform_name;
            uint32_t num_clusters;
            uint32_t num_cores;
            uint32_t num_warps;
            uint32_t num_threads;
            uint32_t num_total_cores;
            uint32_t num_total_warps;
            uint32_t num_total_threads;
        } platinfo;

    } state_;
    

    // Helpers
    void _print_platform_info() const;
    bool _check_transport_connected() const;

    // Parameters
    unsigned wake_dm_retries_    = 10;
    unsigned wake_dm_delay_ms_   = 500;

    // Low-level DM register access
    int dmreg_rd(const DMReg_t &reg, uint32_t &value);
    int dmreg_wr(const DMReg_t &reg, const uint32_t &value);
    int dmreg_rdfield(const DMReg_t &reg, const std::string &fieldname, uint32_t &value);
    int dmreg_wrfield(const DMReg_t &reg, const std::string &fieldname, const uint32_t &value);
    int dmreg_pollfield(const DMReg_t &reg, const std::string &fieldname, const uint32_t &exp_value, uint32_t *final_value,
                        int max_retries, int delay_ms);

    struct DMRegVal_t {
        bool valid = false;
        bool dirty = false;
        uint32_t val;
    };
    std::map<DMReg_t, DMRegVal_t> dmreg_cache_;

    friend class VortexDebugger;
};