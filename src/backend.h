#pragma once
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <cstdint>

#include "dmdefs.h"
#include "util.h"  // Include util.h for RCODE_OK and other return codes

#ifndef DEFAULT_POLL_RETRIES
    #define DEFAULT_POLL_RETRIES 10
#endif
#ifndef DEFAULT_POLL_DELAY_MS
    #define DEFAULT_POLL_DELAY_MS 100
#endif

// Forward declarations
class Transport;
class Logger;
class Backend;

struct BreakPointInfo_t {
    bool enabled = false;       // Is the breakpoint enabled?
    uint32_t addr = 0;          // Address of the breakpoint
    uint32_t replaced_instr;    // Original instruction replaced by breakpoint
    uint32_t hit_count = 0;     // Number of times breakpoint has been hit
};


struct WarpStatus_t {
    int wid;
    bool active;
    bool halted;
    uint32_t pc;
    uint32_t hacause;
};

struct WarpSummary_t {
    bool allhalted;
    bool anyhalted;
    bool allrunning;
    bool anyrunning;
    bool allunavail;
    bool anyunavail;
};

////////////////////////////////////////////////////////////////////////////////
// Backend class
////////////////////////////////////////////////////////////////////////////////
class Backend {
public:
    Backend();
    ~Backend();
    void set_param(const std::string &param, std::string value);
    std::string get_param(const std::string &param) const;

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

    // Sets up the Debug Module
    int setup_dm();

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


    //----- Query Warp Status --------------
    // Returns a map of warp ID to (halted status, PC value)
    int get_warp_status(std::map<int, WarpStatus_t> &warp_status, bool include_pc = true, bool include_hacause = true);
    
    // Check if all/any warps are halted/running
    int get_warp_summary(WarpSummary_t &summary);

    // Get halted state of a specific warp
    int get_warp_state(int wid, bool &halted);

    // Read program counter of selected warp/thread
    int get_warp_pc(uint32_t &pc);
    
    // Write program counter of selected warp/thread
    int set_warp_pc(const uint32_t pc);

    // Get the halt cause of selected warp
    int get_warp_hacause(uint32_t &hacause);


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
    // NOTE: 
    //  - to enable fast injection, it skips selected/halted checks
    //  - Caller must make sure a warp is selected and halted
    int inject_instruction(uint32_t instruction);
    int inject_instruction(const std::string &asm_instr);
    
    
    //----- Platform State Query/Update ----
    // Read/Write GPR value
    int read_gpr(const uint32_t regnum, uint32_t &value);
    int write_gpr(const uint32_t regnum, const uint32_t value);

    // Read/Write the CSR value
    int read_csr(const uint32_t regaddr, uint32_t &value);
    int write_csr(const uint32_t regaddr, const uint32_t value);

    // Read/Write register(GPR/CSR/PC) by name
    int read_reg(const std::string &reg_name, uint32_t &value);
    int write_reg(const std::string &reg_name, uint32_t value);

    // Batch read/write registers by name
    int read_regs(const std::vector<std::string> &reg_names, std::vector<uint32_t> &values);
    int write_regs(const std::vector<std::string> &reg_names, const std::vector<uint32_t> &values);

    // Read/Write memory
    int read_mem(const uint32_t addr, const uint32_t nbytes, std::vector<uint8_t> &data);
    int write_mem(const uint32_t addr, const std::vector<uint8_t> &data);

    // ----- Breakpoint Management -----
    // Set/Remove breakpoints
    int set_breakpoint(uint32_t addr);
    int remove_breakpoint(uint32_t addr);

    // Get breakpoints
    std::unordered_map<uint32_t, BreakPointInfo_t> get_breakpoints() const;

    // Check if any breakpoints are set
    int any_breakpoints(bool &anybps) const;

    // Continue execution until any warp hits a breakpoint
    int until_breakpoint(bool auto_select = false);

    // ----- Accessors -----
    int get_num_warps() const { return state_.platinfo.num_total_warps; }
    int get_num_threads_per_warp() const { return state_.platinfo.num_threads; }

private:
    // Transport 
    Transport *transport_;
    std::string transport_type_;
    Logger *log_;

    // Parameters
    unsigned poll_retries_    = DEFAULT_POLL_RETRIES;
    unsigned poll_delay_ms_   = DEFAULT_POLL_DELAY_MS;
    bool use_emulated_breakpoints_ = false;

    // Current Debugger state
    struct State_t {
        
        // ----- Cache -----
        int selected_wid = -1;
        int selected_tid = -1;
        uint32_t selected_warp_pc = 0;
        // -----------------

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

    std::unordered_map<uint32_t, BreakPointInfo_t> breakpoints_;

    //==============================================================================
    // Helpers
    //==============================================================================
    std::string _get_platform_info_str() const;

    //==============================================================================
    // Low-level DM register access
    //==============================================================================
    int dmreg_rd(const DMReg_t &reg, uint32_t &value);
    int dmreg_wr(const DMReg_t &reg, const uint32_t &value);
    int dmreg_rdfield(const DMReg_t &reg, const std::string &fieldname, uint32_t &value);
    int dmreg_wrfield(const DMReg_t &reg, const std::string &fieldname, const uint32_t &value);
    int dmreg_pollfield(const DMReg_t &reg, const std::string &fieldname, const uint32_t &exp_value, uint32_t *final_value,
                        int max_retries=-1, int delay_ms=-1);

    
    // Friend classes
    friend class VortexDebugger;
    friend class CommandExecutor;
};