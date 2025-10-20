#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

#include "dmdefs.h"

#ifndef DEFAULT_POLL_RETRIES
    #define DEFAULT_POLL_RETRIES 10
#endif
#ifndef DEFAULT_POLL_DELAY_MS
    #define DEFAULT_POLL_DELAY_MS 100
#endif

// Forward declarations
class Transport;
class Logger;


class Backend {
public:
    Backend();
    ~Backend();

    //==========================================================================
    // Transport management
    //==========================================================================

    // Setup transport
    int transport_setup(const std::string &type);

    // Connect/disconnect transport
    int transport_connect(const std::map<std::string, std::string> &args);
    int transport_disconnect();

    // Check transport connection status
    bool transport_connected() const;

    // Initialization
    int initialize(bool quiet=false);

    //==========================================================================
    // API Methods
    //==========================================================================

    //----- DM Control -----------------------
    // Wakes up the Debug Module
    int wake_dm();

    // Resets the target system, optionally halting all warps after reset
    int reset_platform(bool halt=false);

    // Retrieves platform information from the target
    int fetch_platform_info();

    
    //----- Warp Selection -----------------
    // Select specific warps by their warp IDs
    int select_warps(const std::vector<int> &wids);

    // Select all warps, or None if all=false
    int select_warps(bool all);

    // Select a specific global warp and thread for debugging
    int select_warp_thread(int g_wid, int tid);
    
    // Get selected warp and thread IDs
    int get_selected_warp_thread(int &wid, int &tid, bool force_fetch = false);
    int get_selected_warp_pc(uint32_t &pc, bool force_fetch = false);


    //----- Query Warp Status --------------
    // Returns a map of warp ID to (halted status, PC value)
    int get_warp_status(std::map<int, std::pair<bool, uint32_t>> &warp_status, bool include_pc = true);
    
    // Check if all/any warps are halted/running
    int get_warp_summary(bool *allhalted, bool *anyhalted, bool *allrunning, bool *anyrunning);

    // Read program counter of selected warp/thread
    int get_warp_pc(uint32_t &pc);
    
    // Write program counter of selected warp/thread
    int set_warp_pc(const uint32_t pc);


    //----- Warp Control -----------------
    // Halt specific warps
    int halt_warps(const std::vector<int> &wids);
    
    // Halt all warps
    int halt_warps();

    // Resume specific warps
    int resume_warps(const std::vector<int> &wids);

    // Resume all warps
    int resume_warps();

    // Step currently selected warp/thread
    int step_warp();

    // Inject a single instruction into the selected warp/thread
    int inject_instruction(uint32_t instruction);
    int inject_instruction(const std::string &asm_instr);
    
    
    //----- Platform State Query/Update ----
    // Get architecture register value
    int reg_arch_read(const uint32_t regnum, uint32_t &value);

    // Set the architecture register value
    int reg_arch_write(const uint32_t regnum, const uint32_t value);

    // Get the CSR register value
    int reg_csr_read(const uint32_t regaddr, uint32_t &value);

    // Set the CSR register value
    int reg_csr_write(const uint32_t regaddr, const uint32_t value);

    // Read processor register
    int read_register(const std::string &reg_name, uint32_t &value);
    
    // Write processor register
    int write_register(const std::string &reg_name, uint32_t value);

    int mem_read(const uint32_t address, const uint32_t size, std::vector<uint32_t> &data);
    int mem_write(const uint32_t address, const uint32_t size, const std::vector<uint32_t> &data);

private:
    // Transport 
    Transport *transport_;
    std::string transport_type_;
    Logger *log_;

    // Current Debugger state
    struct State_t {
        int selected_wid = -1;
        int selected_tid = -1;
        uint32_t selected_warp_pc = 0;

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
            uint32_t misa;
        } platinfo;

    } state_;
    

    // Helpers
    void _print_platform_info() const;

    // Parameters
    unsigned poll_retries_    = DEFAULT_POLL_RETRIES;
    unsigned poll_delay_ms_   = DEFAULT_POLL_DELAY_MS;

    // Low-level DM register access
    int dmreg_rd(const DMReg_t &reg, uint32_t &value);
    int dmreg_wr(const DMReg_t &reg, const uint32_t &value);
    int dmreg_rdfield(const DMReg_t &reg, const std::string &fieldname, uint32_t &value);
    int dmreg_wrfield(const DMReg_t &reg, const std::string &fieldname, const uint32_t &value);
    int dmreg_pollfield(const DMReg_t &reg, const std::string &fieldname, const uint32_t &exp_value, uint32_t *final_value,
                        int max_retries=-1, int delay_ms=-1);

    struct DMRegVal_t {
        bool valid = false;
        bool dirty = false;
        uint32_t val;
    };
    std::map<DMReg_t, DMRegVal_t> dmreg_cache_;

    friend class VortexDebugger;
};