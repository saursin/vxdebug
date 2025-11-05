#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

#ifndef TRANSPORT_TIMEOUT_MS
    // Default timeout in milliseconds
    #define TRANSPORT_TIMEOUT_MS 1000
#endif

// Forward declarations
class Logger;


// Abstract base class for transport mechanisms (e.g., TCP, Serial, etc.)
class Transport {
public:
    Transport(const std::string &name);
    virtual ~Transport();


    // ----- Connection management -----
    // Connect using parameters in 'args' map
    virtual int connect(const std::map<std::string, std::string> &args) = 0;

    // Disconnect the transport
    virtual int disconnect() = 0;

    // Check if transport is currently connected
    virtual bool is_connected() const = 0;

    // Set communication timeout (in milliseconds)
    void set_timeout(unsigned timeout_ms) { timeout_ms_ = timeout_ms; }

    
    // ----- Low-level buffer send/receive -----
    // Sends data as a string with automatic newline termination.
    virtual int _send_buf(const std::string &data) = 0;
        
    // Receives a line-terminated string into 'data' (newline removed).
    virtual int _recv_buf(std::string &data) = 0;

    // ----- Communication API: Blocking Register Read/Write -----
    // Send a arbitrary command string
    int send_cmd(const std::string &cmd, std::string &response);

    // Handshake
    int handshake();

    // Read a single register
    int read_reg(const uint32_t addr, uint32_t &data);

    // Write a single register
    int write_reg(const uint32_t addr, const uint32_t data);

    // Read multiple registers in a batch
    int read_regs(const std::vector<uint32_t> &addrs, std::vector<uint32_t> &data);

    // Write multiple registers in a batch
    int write_regs(const std::vector<uint32_t> &addrs, const std::vector<uint32_t> &data);


private:
    std::string name_;                                  // Transport name (for logging)

protected:
    Logger *log_;
    unsigned timeout_ms_ = TRANSPORT_TIMEOUT_MS;        // Communication timeout in milliseconds
};



// -----------------------------------------------------------------------------
// TCP Transport implementation
class TCPTransport : public Transport {
public:
    TCPTransport();
    ~TCPTransport();

    int connect(const std::map<std::string, std::string> &args) override;
    int disconnect() override;
    bool is_connected() const override;

    int _send_buf(const std::string &data) override;
    int _recv_buf(std::string &data) override;

private:
    class TCPClient *client_;
    std::string recv_buf_; // Buffer for incoming data
};