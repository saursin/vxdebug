#pragma once
#include <string_view>
#include <cstdint>
#include <stdexcept>

struct FieldInfo_t {
    std::string_view name;
    uint8_t msb;
    uint8_t lsb;
    uint32_t mask() const {
        uint32_t width = msb - lsb + 1;
        if (width == 32) {
            return 0xFFFFFFFF;  // Handle 32-bit field case
        }
        return ((1u << width) - 1) << lsb;
    }
    uint32_t width() const {
        return msb - lsb + 1;
    }
};

struct DMRegInfo_t {
    std::string_view name;
    uint32_t addr;
    const FieldInfo_t* fields;
    size_t num_fields;
};

//------------------------------------------------------------------------------
// DM Register Field Definitions
//------------------------------------------------------------------------------
constexpr FieldInfo_t PLATFORM_FIELDS[] = {
    {"platformid",   31, 28},
    {"numclusters",  27, 21},
    {"numcores",     20, 12},
    {"numwarps",     11,  3},
    {"numthreads",    2,  0}
};

constexpr FieldInfo_t DSELECT_FIELDS[] = {
    {"winsel",       31, 22},
    {"warpsel",      21,  7},
    {"threadsel",     6,  0}
};

constexpr FieldInfo_t WMASK_FIELDS[] = {
    {"mask",         31,  0}
};

constexpr FieldInfo_t WSTATUS_FIELDS[] = {
    {"status",       31,  0}
};

constexpr FieldInfo_t DCTRL_FIELDS[] = {
    {"dmactive",     31, 31},
    {"ndmreset",     30, 30},
    {"allhalted",    29, 29},
    {"anyhalted",    28, 28},
    {"allrunning",   27, 27},
    {"anyrunning",   26, 26},
    {"injectstate",   8,  7},
    {"injectreq",     6,  6},
    {"stepstate",     5,  4},
    {"stepreq",       3,  3},
    {"resethaltreq",  2,  2},
    {"resumereq",     1,  1},
    {"haltreq",       0,  0}
};

constexpr FieldInfo_t DPC_FIELDS[] = {
    {"pc",           31,  0}
};

constexpr FieldInfo_t DINJECT_FIELDS[] = {
    {"instr",        31,  0}
};

constexpr FieldInfo_t DSCRATCH_FIELDS[] = {
    {"data",         31,  0}
};


//------------------------------------------------------------------------------
// DM Register Definitions
//------------------------------------------------------------------------------
enum class DMReg_t : uint8_t {
    PLATFORM,
    DSELECT,
    WMASK,
    WSTATUS,
    DCTRL,
    DPC,
    DINJECT,
    DSCRATCH,
    COUNT
};

constexpr DMRegInfo_t DM_REGS[] = {
    DMRegInfo_t{"platform",  0x00, PLATFORM_FIELDS, std::size(PLATFORM_FIELDS)},
    DMRegInfo_t{"dselect",   0x01, DSELECT_FIELDS,  std::size(DSELECT_FIELDS)},
    DMRegInfo_t{"wmask",     0x02, WMASK_FIELDS,    std::size(WMASK_FIELDS)},
    DMRegInfo_t{"wstatus",   0x03, WSTATUS_FIELDS,  std::size(WSTATUS_FIELDS)},
    DMRegInfo_t{"dctrl",     0x04, DCTRL_FIELDS,    std::size(DCTRL_FIELDS)},
    DMRegInfo_t{"dpc",       0x05, DPC_FIELDS,      std::size(DPC_FIELDS)},
    DMRegInfo_t{"dinject",   0x06, DINJECT_FIELDS,  std::size(DINJECT_FIELDS)},
    DMRegInfo_t{"dscratch",  0x07, DSCRATCH_FIELDS, std::size(DSCRATCH_FIELDS)}
};


constexpr const DMRegInfo_t& get_dmreg(DMReg_t id) noexcept {
    return DM_REGS[static_cast<size_t>(id)];
}

inline const FieldInfo_t* get_dmreg_field(DMReg_t reg, std::string_view fieldname) {
    const auto& rinfo = get_dmreg(reg);
    for (size_t i = 0; i < rinfo.num_fields; ++i) {
        if (rinfo.fields[i].name == fieldname) {
            return &rinfo.fields[i];
        }
    }
    throw std::runtime_error("Invalid field name '" + std::string(fieldname) + "' for register '" + std::string(rinfo.name) + "'");
}

inline uint32_t extract_dmreg_field(DMReg_t reg, std::string_view fieldname, uint32_t reg_value) {
    const auto* finfo = get_dmreg_field(reg, fieldname);
    return (reg_value & finfo->mask()) >> finfo->lsb;
}

inline uint32_t set_dmreg_field(DMReg_t reg, std::string_view fieldname, uint32_t reg_value, uint32_t field_value) {
    const auto* finfo = get_dmreg_field(reg, fieldname);
    uint32_t field_mask = finfo->mask();
    reg_value = (reg_value & ~field_mask) | ((field_value << finfo->lsb) & field_mask);
    return reg_value;
}