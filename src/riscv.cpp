#include "riscv.h"
#include "util.h"

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <cstdint>
#include <array>
#include <filesystem>

#define get_bit(val, pos) (((val) >> (pos)) & 0x1)


std::string rvgpr_num2name(uint32_t regnum) {
    if (regnum < RV_GPR_COUNT) {
        return std::string(RV_GPRS[regnum].abi_name);
    }
    throw std::invalid_argument("Invalid RISC-V GPR number: " + std::to_string(regnum));
}

uint32_t rvgpr_name2num(const std::string &reg_name) {
    for (size_t i=0; i < RV_GPR_COUNT; i++) {
        if (reg_name == RV_GPRS[i].name || reg_name == RV_GPRS[i].abi_name) {
            return RV_GPRS[i].addr;
        }
    }
    throw std::invalid_argument("Invalid RISC-V GPR name: " + reg_name);
}

std::string rvcsr_num2name(uint32_t addr) {
    auto it = RV_CSR_NAMES.find(addr);
    if (it != RV_CSR_NAMES.end()) {
        return std::string(it->second);
    } 
    throw std::invalid_argument("Invalid RISC-V CSR address: 0x" + strfmt("%03X", addr));
}

uint32_t rvcsr_name2addr(const std::string &reg_name) {
    for (const auto& pair : RV_CSR_NAMES) {
        if (reg_name == pair.second) {
            return pair.first;
        }
    }
    throw std::invalid_argument("Invalid RISC-V CSR name: " + reg_name);
}

RVRegType_t rvreg_gettype(const std::string &reg_name) {
    // Check if GPR
    try {
        rvgpr_name2num(reg_name);
        return RVRegType_t::GPR;
    } catch (const std::invalid_argument&) {
        // Not a GPR, continue
    }
    // Check if CSR
    try {
        rvcsr_name2addr(reg_name);
        return RVRegType_t::CSR;
    } catch (const std::invalid_argument&) {
        // Not a CSR, continue
    }
    return RVRegType_t::NONE;
}



std::string rv_isa_string(uint32_t misa, bool verbose) {
    std::string isa_str;

    bool atomic         = get_bit(misa, 0);
    bool bitmanip       = get_bit(misa, 1);
    bool compressed     = get_bit(misa, 2);
    bool doublepr_float = get_bit(misa, 3);
    bool rv32e_base     = get_bit(misa, 4);
    bool singlepr_float = get_bit(misa, 5);
    bool base_isa       = get_bit(misa, 8);
    bool muldiv         = get_bit(misa, 12);
    bool packed_simd    = get_bit(misa, 15);
    bool quadpr_float   = get_bit(misa, 16);
    bool user_mode      = get_bit(misa, 20);
    bool vector         = get_bit(misa, 21);
    bool nonstd_ext     = get_bit(misa, 23);
    uint8_t xlen        = misa >> 30;
    
    std::string xlen_s  = xlen == 1 ? "32" :
                          xlen == 2 ? "64" :
                          xlen == 3 ? "128" : "?";

    // Determine Base ISA and XLEN
    isa_str = strfmt("RV%s", xlen_s.c_str());
    isa_str +=  base_isa   ? "I" : 
                rv32e_base ? "E" : "?";

    // Determine Extensions
    if (muldiv)         isa_str += verbose ? ", MulDiv"                    : "M";
    if (atomic)         isa_str += verbose ? ", Atomic"                  : "A";
    if (singlepr_float) isa_str += verbose ? ", SinglePrecisionFloat"    : "F";
    if (doublepr_float) isa_str += verbose ? ", DoublePrecisionFloat"    : "D";
    if (quadpr_float)   isa_str += verbose ? ", QuadPrecisionFloat"      : "Q";
    if (compressed)     isa_str += verbose ? ", Compressed"              : "C";
    if (bitmanip)       isa_str += verbose ? ", Bitmanip"                : "B";
    if (packed_simd)    isa_str += verbose ? ", PackedSIMD"              : "P";
    if (vector)         isa_str += verbose ? ", Vector"                  : "V";
                        
    isa_str += verbose ? ", CSR" : "_Zicsr";  // CSR regs -> Always present
    
    if (user_mode)      isa_str += verbose ? ", UserMode"                : "";
    if (nonstd_ext)     isa_str += verbose ? ", NonStdExtensionVortex"   : "";

    return isa_str;
}

bool rv_toolchain_check(const std::string &toolchain_prefix) {
    std::string cmd = toolchain_prefix + "-as --version > /dev/null 2>&1";
    int rc = system(cmd.c_str());
    return (rc == 0);
}

// Temporary Directory RAII Wrapper
class TempDir {
public:
    TempDir(std::string template_str) {
        // Ensure pattern ends with XXXXXX (mkdtemp requirement)
        if (template_str.size() < 6 || template_str.substr(template_str.size() - 6) != "XXXXXX")
            template_str += "XXXXXX";
        // mkdtemp requires mutable, null-terminated storage
        std::vector<char> tmpl(template_str.begin(), template_str.end());
        tmpl.push_back('\0');

        char* dir = mkdtemp(tmpl.data());
        if (!dir) {
            throw std::runtime_error("Failed to create temporary directory");
        }
        this->dir = std::string(dir);
    }

    ~TempDir() {
        std::error_code ec;                    // avoid throwing in destructor
        std::filesystem::remove_all(dir, ec);  // best-effort cleanup
    }

    std::string dir;    // final path to temporary directory
};


// Assemble RISC-V assembly instructions into machine code words
// NOTE: 
//  - assumes all instructions are 32-bit (RVC disabled)
//  - all lines are independent (no labels or branches)
//  - doesn't handle pseudo-instructions, need to be expanded by user
std::vector<uint32_t> rv_asm(const std::vector<std::string> &asm_lines, const std::string &toolchain_prefix) {
    static std::unordered_map<std::string, uint32_t> __rvasm_cache;

    std::vector<uint32_t> machine_code;
    machine_code.resize(asm_lines.size(), 0);  // Pre-allocate space for machine code

    // Identify which lines need to be assembled
    std::vector<size_t> to_assemble_indices;
    
    // ---------- Check Cache ----------
    for (size_t i = 0; i < asm_lines.size(); ++i) {
        const auto &line = asm_lines[i];
        auto it = __rvasm_cache.find(line);
        if (it != __rvasm_cache.end()) {
            // Cache hit: use cached value
            machine_code[i] = it->second;
            // printf("ASMCache hit: %s => 0x%08X\n", line.c_str(), it->second);
        } else {
            // Cache miss
            to_assemble_indices.push_back(i);
            // printf("ASMCache miss: %s\n", line.c_str());
            // print asm cache
            // for (const auto& pair : __rvasm_cache) {
            //     printf("  %s => 0x%08X\n", pair.first.c_str(), pair.second);
            // }
        }
    }
    
    // If all lines were in cache, return early
    if (to_assemble_indices.empty()) {
        return machine_code;
    }

    // ---------- Assemble missing lines ----------
    int rc;
    {// Temporary Directory Scope
        // Create a temporary directory for assembly files
        TempDir temp_dir("/tmp/vxdebug");
        std::string asm_file = temp_dir.dir + "/temp.S";
        std::string obj_file = temp_dir.dir + "/temp.o";
        std::string bin_file = temp_dir.dir + "/temp.bin";

        {   // Write assembly file
            std::ofstream asm_stream(asm_file);
            if (!asm_stream) throw std::runtime_error("Failed to create file: " + asm_file);           
            asm_stream << ".option push\n"
                        ".option norvc\n"   // Force 32-bit instructions
                        ".text\n"
                        ".balign 4\n"       // Align to 4 bytes
                        ".globl _start\n"
                        "_start:\n";
            // add lines to assemble from assemble_indices
            for (size_t idx : to_assemble_indices) {
                asm_stream << asm_lines[idx] << "\n";
            }
            asm_stream << ".option pop\n\n";
            if (!asm_stream) throw std::runtime_error("Failed to write to file: " + asm_file);
        } // RAII: file automatically closed when asm_stream goes out of scope

        // Assemble using RISC-V assembler
        rc = std::system(strfmt("%s-as %s -o %s", toolchain_prefix.c_str(), asm_file.c_str(), obj_file.c_str()).c_str());
        if (rc != 0) throw std::runtime_error("Failed to assemble file: " + asm_file);
        
        // Convert to binary
        rc = std::system(strfmt("%s-objcopy -O binary %s %s", toolchain_prefix.c_str(), obj_file.c_str(), bin_file.c_str()).c_str());
        if (rc != 0) throw std::runtime_error("Failed to convert object file to binary: " + obj_file);
        
        { // Read binary file
            std::ifstream bin_stream(bin_file, std::ios::binary);
            if (!bin_stream) throw std::runtime_error("Failed to open binary file: " + bin_file);
                        
            std::array<uint8_t, 4> bytes;
            size_t n = 0;
            while (bin_stream.read(reinterpret_cast<char*>(bytes.data()), 4)) {
                // Little-endian byte order assembly
                uint32_t instr = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
                size_t orig_idx = to_assemble_indices[n++];
                // Store in machine_code vector, update cache
                machine_code[orig_idx] = instr;
                __rvasm_cache[asm_lines[orig_idx]] = instr;
            }
            
            // Check if we read a partial instruction
            if (bin_stream.gcount() > 0) {
                throw std::runtime_error("Incomplete instruction in binary file");
            }

            // Check if input lines == assembled instructions
            if (n != to_assemble_indices.size()) {
                throw std::runtime_error("Mismatch in number of assembled instructions, Did you use a pseudo-instruction?");
            }
        }
    }// End Temporary Directory Scope

    return machine_code;
}


