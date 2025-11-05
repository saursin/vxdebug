#include "transport.h"
#include "logger.h"
#include "tcputils.h"
#include "util.h"

#include <cstring>
#include <chrono>
#include <thread>

#define MAX_BATCH_SZ 8

//==============================================================================
// Transport Base Class
//==============================================================================

Transport::Transport(const std::string &name):
    name_(name),
    log_(new Logger(name + "Transport", 5))
{}

Transport::~Transport() {
    delete log_;
}

int Transport::handshake() {
    log_->debug("Performing handshake with debug server...");
    int rc = _send_buf("p");
    if (rc != RCODE_OK) {
        log_->error("Failed to send handshake message");
        return rc;
    }

    std::string rbuf;
    rc = _recv_buf(rbuf);
    if (rc != RCODE_OK) {
        log_->error("Failed to receive handshake response");
        return rc;
    }

    if (rbuf != "+P") {
        log_->error("Invalid handshake response: " + rbuf);
        return RCODE_ERROR;
    }

    log_->info("Handshake successful");
    return RCODE_OK;
}

int Transport::send_cmd(const std::string &cmd, std::string &response) {
    int rc = _send_buf(cmd);
    if (rc != RCODE_OK) {
        log_->error("Failed to send command: " + cmd);
        return rc;
    }
    rc = _recv_buf(response);
    if (rc != RCODE_OK) {
        log_->error("Failed to receive command response for: " + cmd);
        return rc;
    }
    if((response)[0] != '+') {
        log_->error("Command '" + cmd + "' failed (got NACK)");
        return RCODE_ERROR;
    }   
    return RCODE_OK;
}

int Transport::read_reg(const uint32_t addr, uint32_t &data) {
    int rc = RCODE_OK;
   
    // Send read command (fmt: "rXXXX")
    std::string sbuf = strfmt("r%04x", addr);
    rc = _send_buf(sbuf);
    if (rc != RCODE_OK)  return rc;

    // Receive response (fmt: "+XXXXXXXX" or "-")
    std::string rbuf;
    rc = _recv_buf(rbuf);
    if (rc != RCODE_OK) { return rc; }

    // Parse response
    if(rbuf[0] == '+') {
        if (rbuf.length() != 9) {
            log_->error("Invalid register read response length");
            return RCODE_ERROR;
        }
        data = std::strtoul(rbuf.c_str() + 1, nullptr, 16);
    } 
    else if(rbuf[0] == '-') {
        log_->error("Register read failed (got NACK)");
        return RCODE_ERROR;
    } 
    else {
        log_->error("Failed to parse register read response");
        return RCODE_ERROR;
    }
    return RCODE_OK;
}

int Transport::write_reg(const uint32_t addr, const uint32_t data) {
    int rc = RCODE_OK;

    // Send write command (fmt: "wXXXX:XXXXXXXX")
    std::string sbuf = strfmt("w%04x:%08x", addr, data);
    rc = _send_buf(sbuf);
    if (rc != RCODE_OK) return rc;

    // Receive response (fmt: "+" or "-")
    std::string rbuf;
    rc = _recv_buf(rbuf);
    if (rc != RCODE_OK) return rc;

    if(rbuf[0] == '+') {
        return RCODE_OK;
    }
    else if(rbuf[0] == '-') {
        log_->error("Register write failed (got NACK)");
        return RCODE_ERROR;
    }
    else {
        log_->error("Failed to parse register write response");
        return RCODE_ERROR;
    }
    return RCODE_OK;
}

int Transport::read_regs(const std::vector<uint32_t> &addrs, std::vector<uint32_t> &data) {
    if(addrs.empty()) return RCODE_OK;

    size_t n = addrs.size();
    if(n > MAX_BATCH_SZ) {
        log_->error("Too many addresses in batch read");
        return RCODE_BUFFER_OVRFLW;
    }

    // Build read command (fmt: "RXXXX,XXXX,XXXX")
    std::string sbuf = "R";
    for(size_t i = 0; i < n; ++i) {
        sbuf += strfmt("%04x", addrs[i]);
        if(i != n - 1)
            sbuf += ",";
    }
    int rc = _send_buf(sbuf);
    if (rc != RCODE_OK) { return rc; }

    // Receive response (fmt: "+XXXXXXXX,XXXXXXXX" or "-")
    std::string rbuf;
    rc = _recv_buf(rbuf);
    if (rc != RCODE_OK) { return rc; }

    // Parse response
    if(rbuf[0] == '+') {
        std::vector<std::string> tokens = tokenize(rbuf.substr(1), ',');
        if(tokens.size() != n) {
            log_->error("Batch read response size mismatch");
            return RCODE_INVALID_ARG;
        }
        data.clear();
        for(const auto& tok : tokens) {
            data.push_back(std::strtoul(tok.c_str(), nullptr, 16));
        }
    } else if(rbuf[0] == '-') {
        log_->error("Batch register read failed (got NACK)");
        return RCODE_ERROR;
    } else {
        log_->error("Failed to parse batch register read response");
        return RCODE_ERROR;
    }
    return RCODE_OK;
}

int Transport::write_regs(const std::vector<uint32_t> &addrs, const std::vector<uint32_t> &data) {
    if(addrs.empty()) return RCODE_OK;
    if(addrs.size() != data.size()) {
        log_->error("Address and data count mismatch for batch write");
        return RCODE_INVALID_ARG;
    }

    size_t n = addrs.size();
    if(n > MAX_BATCH_SZ) {
        log_->error("Too many addresses in batch write");
        return RCODE_BUFFER_OVRFLW;
    }
    
    // Build command: "W<addr1>,<addr2>:<val1>,<val2>"
    std::string sbuf = "W";   
    for(size_t i = 0; i < n; ++i) {
        sbuf += strfmt("%04x", addrs[i]);
        if(i != n - 1)
            sbuf += ",";
    }
    sbuf += ";";
    for(size_t i = 0; i < n; ++i) {
        sbuf += strfmt("%08x", data[i]);
        if(i != n - 1)
            sbuf += ",";
    }

    int rc = _send_buf(sbuf);
    if (rc != RCODE_OK) {
        log_->error("Failed to send batch write command");
        return RCODE_ERROR;
    }

    // Receive response (fmt: "+" or "-")
    std::string rbuf;
    rc = _recv_buf(rbuf);
    if (rc != RCODE_OK) {
        log_->error("Failed to receive batch write response");
        return RCODE_ERROR;
    }

    if(rbuf[0] == '+') {
        return RCODE_OK;
    }
    else if(rbuf[0] == '-') {
        log_->error("Batch register write failed (got NACK)");
        return RCODE_ERROR;
    }
    else {
        log_->error("Failed to parse batch write response");
        return RCODE_ERROR;
    }
}



// =============================================================================
// TCP Transport Implementation
// =============================================================================

TCPTransport::TCPTransport():
    Transport("TCP"),
    client_(new TCPClient()),
    recv_buf_()
{}

TCPTransport::~TCPTransport() {
    delete client_;     // Destructor handles disconnect if needed
}

int TCPTransport::connect(const std::map<std::string, std::string> &args) {
    if (args.find("ip") == args.end() || args.find("port") == args.end()) {
        log_->error("TCPTransport requires 'ip' and 'port' arguments");
        return RCODE_INVALID_ARG;
    }
    try {
        std::string ip = args.at("ip");
        uint16_t port = static_cast<uint16_t>(std::stoi(args.at("port")));
        client_->connect(ip, port);
    } catch (const std::exception &e) {
        log_->error("Connection failed: " + std::string(e.what()));
        return RCODE_ERROR;
    }
    log_->info("Connected to " + client_->get_ip() + ":" + std::to_string(client_->get_port()));
    return RCODE_OK;
}

int TCPTransport::disconnect() {
    client_->disconnect();
    log_->info("Disconnected from " + client_->get_ip() + ":" + std::to_string(client_->get_port()));
    return RCODE_OK;
}

bool TCPTransport::is_connected() const {
    return client_->is_connected();
}

int TCPTransport::_send_buf(const std::string &data) {
    if (!client_->is_connected()) return RCODE_ERROR;
    if (data.empty()) return RCODE_OK;

    // Ensure newline termination
    std::string msg = data; // Make a copy to modify
    if (msg.back() != '\n') {
        msg.push_back('\n');
    }

    try {
        client_->send_data(msg.c_str(), msg.size());
        log_->debug("TX: " + data);
    } catch (const std::exception& e) {
        log_->error("Send failed: " + std::string(e.what()));
        return RCODE_ERROR;
    }
    return RCODE_OK;
}


int TCPTransport::_recv_buf(std::string &out) {
    if (!client_->is_connected()) return RCODE_ERROR;
    out.clear();

    // Add timeout logic using the configured timeout
    auto start_time = std::chrono::steady_clock::now();
    const auto timeout_duration = std::chrono::milliseconds(timeout_ms_);

    while (true) {
        // See if we already have a full line
        size_t pos = recv_buf_.find('\n');
        if (pos != std::string::npos) {
            out = recv_buf_.substr(0, pos);
            recv_buf_.erase(0, pos + 1);
            log_->debug("RX: " + out);
            return RCODE_OK;
        }

        // Otherwise, read more data
        char tmp[256];
        ssize_t n = 0;
        try {
            n = client_->recv_data(tmp, sizeof(tmp));
        } catch (const std::exception& e) {
            log_->error("Receive failed: " + std::string(e.what()));
            return RCODE_ERROR;
        }

        if (n > 0) {
            recv_buf_.append(tmp, n);
        } else {
            // No new data — check timeout
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (elapsed > timeout_duration) {
                log_->error("Receive timeout - no response from server");
                return RCODE_TIMEOUT;
            }
            
            if (!client_->is_connected()) {
                log_->error("Client disconnected while waiting for data");
                return RCODE_TRANSPORT_ERR;
            }
            
            // Small delay before retrying
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}