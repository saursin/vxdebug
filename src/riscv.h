#pragma once
#include <string_view>
#include <stdexcept>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_map>

#ifndef RISCV_TOOLCHAIN_PREFIX
    #define RISCV_TOOLCHAIN_PREFIX "riscv64-unknown-elf"
#endif

enum RVRegType_t {RV_GPR, RV_CSR};

struct RVRegInfo_t {
    std::string_view name;
    uint32_t addr;
};

constexpr RVRegInfo_t RV_GPRS[] = {
    {"x0",  0},   {"x1",  1},   {"x2",  2},   {"x3",  3},   {"x4",  4},   {"x5",  5},   {"x6",  6},   {"x7",  7},
    {"x8",  8},   {"x9",  9},   {"x10", 10},  {"x11", 11},  {"x12", 12},  {"x13", 13},  {"x14", 14},  {"x15", 15},
    {"x16", 16},  {"x17", 17},  {"x18", 18},  {"x19", 19},  {"x20", 20},  {"x21", 21},  {"x22", 22},  {"x23", 23},
    {"x24", 24},  {"x25", 25},  {"x26", 26},  {"x27", 27},  {"x28", 28},  {"x29", 29},  {"x30", 30},  {"x31", 31}
};
constexpr size_t RV_GPR_COUNT = std::size(RV_GPRS);

enum RV_CSR: uint32_t {
    // Floating Point CSRs
    RV_CSR_FFLAGS            = 0x001,
    RV_CSR_FRM               = 0x002,
    RV_CSR_FCSR              = 0x003,

    // Machine CSRs
    RV_CSR_MISA              = 0x301,
    RV_CSR_MSCRATCH          = 0x340,

    // Machine Performance CSRs
    RV_CSR_MCYCLE            = 0xb00,
    RV_CSR_MCYCLEH           = 0xb80,
    RV_CSR_MINSTRET          = 0xb02,
    RV_CSR_MINSTRETH         = 0xb82,

    RV_CSR_MVENDORID         = 0xf11,
    RV_CSR_MARCHID           = 0xf12,
    RV_CSR_MIMPID            = 0xf13,

    // Vortex-specific CSRs
    RV_CSR_VX_WARP_ID        = 0xcc1,
    RV_CSR_VX_CORE_ID        = 0xcc2,
    RV_CSR_VX_ACTIVE_WARPS   = 0xcc3,
    RV_CSR_VX_ACTIVE_THREADS = 0xcc4,

    RV_CSR_VX_NUM_THREADS    = 0xfc0,
    RV_CSR_VX_NUM_WARPS      = 0xfc1,
    RV_CSR_VX_NUM_CORES      = 0xfc2,
    RV_CSR_VX_LOCAL_MEM_BASE = 0xfc3,

    // Debug CSRs
    RV_CSR_VX_DSCRATCH       = 0x7b2
};

const std::unordered_map<uint32_t, std::string_view> RV_CSR_NAMES = {
    {RV_CSR_FFLAGS,            "fflags"},
    {RV_CSR_FRM,               "frm"},
    {RV_CSR_FCSR,              "fcsr"},
    {RV_CSR_MISA,              "misa"},
    {RV_CSR_MSCRATCH,          "mscratch"},
    {RV_CSR_MCYCLE,            "mcycle"},
    {RV_CSR_MCYCLEH,           "mcycleh"},
    {RV_CSR_MINSTRET,          "minstret"},
    {RV_CSR_MINSTRETH,         "minstreth"},
    {RV_CSR_MVENDORID,         "mvendorid"},
    {RV_CSR_MARCHID,           "marchid"},
    {RV_CSR_MIMPID,            "mimpid"},
    {RV_CSR_VX_WARP_ID,        "vx_warp_id"},
    {RV_CSR_VX_CORE_ID,        "vx_core_id"},
    {RV_CSR_VX_ACTIVE_WARPS,   "vx_active_warps"},
    {RV_CSR_VX_ACTIVE_THREADS, "vx_active_threads"},
    {RV_CSR_VX_NUM_THREADS,    "vx_num_threads"},
    {RV_CSR_VX_NUM_WARPS,      "vx_num_warps"},
    {RV_CSR_VX_NUM_CORES,      "vx_num_cores"},
    {RV_CSR_VX_LOCAL_MEM_BASE, "vx_local_mem_base"},
    {RV_CSR_VX_DSCRATCH,       "vx_dscratch"}
};

// Get CSR name from address
inline std::string rvcsr_getname(uint32_t addr);

// Get human-readable ISA string from MISA CSR value
std::string rv_isa_string(uint32_t misa, bool verbose=false);



// Check if RISC-V toolchain is available
bool rv_toolchain_check(const std::string &toolchain_prefix=RISCV_TOOLCHAIN_PREFIX);

// Assemble RISC-V assembly lines into machine code words
std::vector<uint32_t> rv_asm(const std::vector<std::string> &asm_lines, const std::string &toolchain_prefix=RISCV_TOOLCHAIN_PREFIX);

