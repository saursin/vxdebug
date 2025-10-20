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

#define get_bit(val, pos) (((val) >> (pos)) & 0x1)

std::string rvcsr_getname(uint32_t addr) {
    auto it = RV_CSR_NAMES.find(addr);
    if (it != RV_CSR_NAMES.end()) {
        return std::string(it->second);
    } else {
        return strfmt("csr_0x%03X", addr);
    }
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

std::vector<uint32_t> rv_asm(const std::vector<std::string> &asm_lines, const std::string &toolchain_prefix) {
    // Create a temporary directory for assembly files
    std::string temp_dir = "/tmp/vxdebug_rvasm/";
    std::string asm_file = temp_dir + "temp.S";
    std::string obj_file = temp_dir + "temp.o";
    std::string bin_file = temp_dir + "temp.bin";

    int rc;
    // Create temporary directory

    rc = system(("mkdir -p " + temp_dir).c_str());
    if (rc != 0) {
        throw std::runtime_error("Failed to create dir: " + temp_dir);
    }

    // Write assembly file using modern C++ streams
    {
        std::ofstream asm_stream(asm_file);
        if (!asm_stream) {
            throw std::runtime_error("Failed to create file: " + asm_file);
        }
        
        asm_stream << ".option push\n"
                      ".option norvc\n"   // Force 32-bit instructions
                      ".text\n"
                      ".balign 4\n"       // Align to 4 bytes
                      ".globl _start\n"
                      "_start:\n";
        
        for (const auto &line : asm_lines) {
            asm_stream << line << "\n";
        }
        asm_stream << ".option pop\n";
        
        if (!asm_stream) {
            throw std::runtime_error("Failed to write to file: " + asm_file);
        }
    } // RAII: file automatically closed when asm_stream goes out of scope

    // Assemble using RISC-V toolchain
    rc = system(strfmt("%s-as %s -o %s", toolchain_prefix.c_str(), asm_file.c_str(), obj_file.c_str()).c_str());
    if (rc != 0) {
        throw std::runtime_error("Failed to assemble file: " + asm_file);
    }
    // Convert to binary
    rc = system(strfmt("%s-objcopy -O binary %s %s", toolchain_prefix.c_str(), obj_file.c_str(), bin_file.c_str()).c_str());
    if (rc != 0) {
        throw std::runtime_error("Failed to convert object file to binary: " + obj_file);
    }
    // Read binary file using modern C++ streams
    std::vector<uint32_t> machine_code;
    {
        std::ifstream bin_stream(bin_file, std::ios::binary);
        if (!bin_stream) {
            throw std::runtime_error("Failed to open binary file: " + bin_file);
        }
        
        std::array<uint8_t, 4> bytes;
        while (bin_stream.read(reinterpret_cast<char*>(bytes.data()), 4)) {
            // Little-endian byte order assembly
            uint32_t instr = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
            machine_code.push_back(instr);
        }
        
        // Check if we read a partial instruction
        if (bin_stream.gcount() > 0) {
            throw std::runtime_error("Incomplete instruction in binary file");
        }
    } // RAII: file automatically closed when bin_stream goes out of scope
    
    return machine_code;
}


