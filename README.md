# VxDebug
Debugger For Vortex GPGPU

**Vortex Debug System Overview**
```mermaid
flowchart TD 
    user[👤 User]
    p1[🌐TCP:3333]
    gdb["💻 RISC-V GDB<br/>(client)"]
    console[💻 Vxdbg Console]
    subgraph vxdbg[Vortex Debugger]
        Ds["🖥️ GDB Stub (server)"]
        Df[Frontend] 
        Db[Backend]
        Dt["Transport (client)"]
        Ds -- "vxdbg commands" --> Df
        Df -- "Backend API" --> Db
        Db -- "R/W DMReg" --> Dt
    end
    
    p2[🌐TCP:5555]
    user .-> console -- "vxdbg commands" --> Df
    user .-> gdb --> p1 -- "RSP commands" --> Ds
    Dt -- "Transport API" --> p2
    
    %% RTLSIm 
    subgraph rtlsim[RTLSim]
        Rs[🖥️ Debug Server]
        Rb[Bus Driver]
        Rm["RTL Simulation<br/>(Vortex)"]
        Rs --> Rb --> Rm
    end
    
    p2 .-> Rs
    
    %% XrtSim
    subgraph xrtruntime[Vortex XRT Runtime]
        Xrs[🖥️ Debug Server]
        Xrt[Vortex XRT Runtime]
        Xrs --> Xrt
    end
    
    subgraph xrtsim[XRTSim]
        Xsa[AXI Driver]
        Xsm["RTL Simulation<br/>(Vortex AFU)"]
        Xsa --> Xsm 
    end
    
    Xrt .->|XRT API| Xsa 
    
    %% FPGA
    subgraph F[FPGA]
        Fxr[XRT Runtime]
        Fxd["FPGA Device<br/>(Vortex AFU)"]
        Fxr -- "PCIe" --> Fxd
    end
    Xrt .-> |XRT API| Fxr
    
    p2 .-> Xrs
```






# Prerequisites
```bash
# Install readline (optional)
sudo apt install libreadline
```

# Build/Install Instructions
```bash
# Building the debugger
make all
```
- Use `DEBUG=1` to build with debug flags.
- Use `READLINE=0` to build without readline.

```bash
# Install to a specific path (default `$HOME/opt/bin`).
make PREFIX=<install-path> install

# Add path to bashrc
echo "export PATH=\$HOME/opt/bin:\$PATH" >> ~/.bashrc
```

# Usage
We use a config script (`vxdbg.cfg`) to perform initialization, start GDB server, and connect transport.

```bash
vxdbg -f vxdbg.cfg
```

- use `-v 5` to view all debug messages.
