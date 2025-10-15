#pragma once
#include <string>
#include <vector>
#include <cstdint>

#ifndef TCPCLIENT_TIMEOUT_MS
    #define TCPCLIENT_TIMEOUT_MS 5000
#endif

#ifndef TCPSERVER_TIMEOUT_MS
    #define TCPSERVER_TIMEOUT_MS 5000
#endif

//==============================================================================
// Simple TCP client wrapper
//==============================================================================
class TCPClient {
public:
    TCPClient();
    ~TCPClient();

    // Connect to server with optional timeout (ms)
    void connect(const std::string& ip, uint16_t port, unsigned timeout_ms = TCPCLIENT_TIMEOUT_MS);
    
    // Disconnect from server
    void disconnect();

    // Check if connected
    bool is_connected() const { return connected_; }

    // Get connection details
    std::string get_ip() const { return ip_; }
    uint16_t get_port() const  { return port_; }

    // Send/Receive data
    ssize_t send_data(const char* buf, size_t len);
    ssize_t recv_data(char* buf, size_t maxlen);

private:
    std::string ip_;
    uint16_t port_;
    int sockfd_;
    bool connected_;
};


//==============================================================================
// Simple TCP Server wrapper
//==============================================================================
class TCPServer {
public:
    TCPServer();
    ~TCPServer();

    // Start server on specified port
    void start(uint16_t port);

    // Accept a client connection with optional timeout (ms) (**blocking**)
    void accept_client(unsigned timeout_ms = TCPSERVER_TIMEOUT_MS);

    // Stop server
    void stop();

    // Check if server is running
    bool is_running() const { return running_; }

    // Get server port
    uint16_t get_port() const { return port_; }

    // Send/Receive data
    ssize_t send_data(const char *buf, size_t len);
    ssize_t recv_data(char *buf, size_t maxlen);

private:
    uint16_t port_;
    int server_fd_;
    int client_fd_;
    bool running_;
};
