#pragma once
#include <string_view>
#include <stdexcept>

#ifndef RISCV_TOOLCHAIN_PREFIX
    #define RISCV_TOOLCHAIN_PREFIX "riscv64-unknown-elf"
#endif

enum RVRegType {GPR, CSR};

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

constexpr RVRegInfo_t RV_CSRS[] = {
    {"vendorid",          0xf11},
    {"archid",            0xf12},
    {"impid",             0xf13},
    {"hartid",            0xf14},
    {"mcycle",            0xb00},
    {"mcycleh",           0xb80},
    {"instret",           0xb02},
    {"instreth",          0xb82},
    {"vx_thread_id",      0xCC0},
    {"vx_warp_id",        0xCC1},
    {"vx_core_id",        0xCC2},
    {"vx_active_warps",   0xCC3},
    {"vx_active_threads", 0xCC4},
    {"vx_num_threads",    0xFC0},
    {"vx_num_warps",      0xFC1},
    {"vx_num_cores",      0xFC2},
    {"vx_local_mem_base", 0xFC3},
    {"vx_dscratch",       0x7B2}
};
constexpr size_t RV_CSRS_COUNT = std::size(RV_CSRS);

inline uint32_t rvreg_name2addr(RVRegType type, const std::string &name) {
    const RVRegInfo_t* reg_array = (type == GPR) ? RV_GPRS : RV_CSRS;
    size_t reg_count = (type == GPR) ? RV_GPR_COUNT : RV_CSRS_COUNT;
    for (size_t i = 0; i < reg_count; ++i) {
        if (reg_array[i].name == name) {
            return reg_array[i].addr;
        }
    }
    throw std::runtime_error("Invalid register name '" + name + "'");
}

inline std::string rvreg_addr2name(RVRegType type, uint32_t addr) {
    const RVRegInfo_t* reg_array = (type == GPR) ? RV_GPRS : RV_CSRS;
    size_t reg_count = (type == GPR) ? RV_GPR_COUNT : RV_CSRS_COUNT;
    for (size_t i = 0; i < reg_count; ++i) {
        if (reg_array[i].addr == addr) {
            return std::string(reg_array[i].name);
        }
    }
    throw std::runtime_error("Invalid register address '0x" + std::to_string(addr) + "'");
}