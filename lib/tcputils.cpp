#include "tcputils.h"

#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <fcntl.h>
#include <sys/socket.h>
#include <errno.h>
#include <sys/select.h>
#include <netinet/in.h>


//==============================================================================
// TCP Client Implementation
//==============================================================================
TCPClient::TCPClient():
    ip_(""),
    port_(0),
    sockfd_(-1),
    connected_(false)
{}

TCPClient::~TCPClient() {
    disconnect();
}

void TCPClient::connect(const std::string& ip, uint16_t port, unsigned timeout_ms) {
    if (connected_) {
        return; // Already connected
    }
    ip_ = ip;
    port_ = port;

    sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0) {
        throw std::runtime_error("Socket creation failed: " + std::string(strerror(errno)));
    }

    // Set socket to non-blocking
    int flags = fcntl(sockfd_, F_GETFL, 0);
    if (flags < 0) {
        close(sockfd_);
        throw std::runtime_error("Failed to get socket flags: " + std::string(strerror(errno)));
    }
    
    if (fcntl(sockfd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(sockfd_);
        throw std::runtime_error("Failed to set socket non-blocking: " + std::string(strerror(errno)));
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) <= 0) {
        close(sockfd_);
        throw std::runtime_error("Invalid IP address: " + ip_);
    }

    int ret = ::connect(sockfd_, (sockaddr*)&addr, sizeof(addr));
    if (ret < 0) {
        if (errno == EINPROGRESS) {
            // Connection is in progress, wait using select()
            fd_set fdset;
            FD_ZERO(&fdset);
            FD_SET(sockfd_, &fdset);

            if (timeout_ms > 0) {
                // Use timeout
                timeval tv;
                tv.tv_sec = timeout_ms / 1000;
                tv.tv_usec = (timeout_ms % 1000) * 1000;
                ret = select(sockfd_ + 1, nullptr, &fdset, nullptr, &tv);
            } else {
                // No timeout - wait indefinitely
                ret = select(sockfd_ + 1, nullptr, &fdset, nullptr, nullptr);
            }

            if (ret <= 0) {
                close(sockfd_);
                sockfd_ = -1;
                if (ret == 0) {
                    throw std::runtime_error("Connection timeout");
                } else {
                    throw std::runtime_error("Select error: " + std::string(strerror(errno)));
                }
            }

            // Check for connection errors
            int so_error;
            socklen_t len = sizeof(so_error);
            if (getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0) {
                close(sockfd_);
                sockfd_ = -1;
                throw std::runtime_error("Failed to get socket error: " + std::string(strerror(errno)));
            }
            
            if (so_error != 0) {
                close(sockfd_);
                sockfd_ = -1;
                throw std::runtime_error("Connection failed: " + std::string(strerror(so_error)));
            }
        } else {
            close(sockfd_);
            sockfd_ = -1;
            throw std::runtime_error("Connection failed: " + std::string(strerror(errno)));
        }
    }

    // Restore socket to blocking
    if (fcntl(sockfd_, F_SETFL, flags) < 0) {
        close(sockfd_);
        sockfd_ = -1;
        throw std::runtime_error("Failed to restore socket blocking mode: " + std::string(strerror(errno)));
    }

    connected_ = true;
}

void TCPClient::disconnect() {
    if (connected_) {
        close(sockfd_);
        sockfd_ = -1;
        connected_ = false;
    }
}

ssize_t TCPClient::send_data(const char* buf, size_t len) {
    if (!connected_) {
        throw std::runtime_error("Client not connected");
    }
    
    if (buf == nullptr || len == 0) {
        return 0;
    }

    ssize_t total_sent = 0;
    ssize_t remaining = len;
    
    while (remaining > 0) {
        ssize_t sent = send(sockfd_, buf + total_sent, remaining, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket would block, try again
                continue;
            }
            throw std::runtime_error("Send failed: " + std::string(strerror(errno)));
        }
        if (sent == 0) {
            throw std::runtime_error("Connection closed");
        }
        
        total_sent += sent;
        remaining -= sent;
    }

    return total_sent;
}

ssize_t TCPClient::recv_data(char* buf, size_t maxlen) {
    if (!connected_) {
        throw std::runtime_error("Client not connected");
    }

    if (buf == nullptr || maxlen == 0) {
        return 0;
    }

    ssize_t received = recv(sockfd_, buf, maxlen, 0);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0; // No data available
        }
        throw std::runtime_error("Receive failed: " + std::string(strerror(errno)));
    }
    if (received == 0) {
        // Connection closed by peer
        connected_ = false;
        return 0;
    }

    return received;
}



//==============================================================================
// TCP Server Implementation
//==============================================================================
TCPServer::TCPServer() :
    port_(0),
    server_fd_(-1),
    client_fd_(-1),
    running_(false)
{}

TCPServer::~TCPServer() {
    stop();
}

void TCPServer::start(uint16_t port) {
    if (running_) {
        return; // Already running
    }
    port_ = port;

    // Create socket
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        throw std::runtime_error("Failed to create server socket");
    }

    // Set socket options to reuse address
    int opt = 1;
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(server_fd_);
        throw std::runtime_error("Failed to set socket options");
    }

    // Setup server address
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    // Bind socket to address
    if (bind(server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server_fd_);
        throw std::runtime_error("Failed to bind socket to port " + std::to_string(port_));
    }

    // Start listening
    if (listen(server_fd_, 5) < 0) {
        close(server_fd_);
        throw std::runtime_error("Failed to listen on socket");
    }

    running_ = true;
}

void TCPServer::accept_client(unsigned timeout_ms) {
    if (!running_) {
        throw std::runtime_error("Server is not running");
    }

    if (timeout_ms > 0) {
        // Use select for timeout
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd_, &readfds);

        timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int ret = select(server_fd_ + 1, &readfds, nullptr, nullptr, &tv);
        if (ret < 0) {
            throw std::runtime_error("Select failed: " + std::string(strerror(errno)));
        }
        if (ret == 0) {
            throw std::runtime_error("Server accept timeout");
        }
    }
    // When timeout_ms == 0, we don't use select and just call accept() directly
    // This will block indefinitely until a connection is available

    sockaddr_in client_addr = {};
    socklen_t client_len = sizeof(client_addr);
    client_fd_ = accept(server_fd_, (sockaddr*)&client_addr, &client_len);
    if (client_fd_ < 0) {
        throw std::runtime_error("Failed to accept client: " + std::string(strerror(errno)));
    }
}

void TCPServer::stop() {
    if (client_fd_ >= 0) {
        close(client_fd_);
        client_fd_ = -1;
    }
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }
    running_ = false;
}

ssize_t TCPServer::send_data(const char* buf, size_t len) {
    if (!running_) throw std::runtime_error("Server not running");  
    
    if (buf == nullptr || len == 0) {
        return 0;
    }

    ssize_t total_sent = 0;
    ssize_t remaining = len;
    
    while (remaining > 0) {
        ssize_t sent = send(client_fd_, buf + total_sent, remaining, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket would block, try again
                usleep(1000); // sleep 1ms before retry
                continue;
            }
            throw std::runtime_error("Send failed: " + std::string(strerror(errno)));
        }
        if (sent == 0) {
            throw std::runtime_error("Connection closed");
        }
        
        total_sent += sent;
        remaining -= sent;
    }

    return total_sent;
}

ssize_t TCPServer::recv_data(char* buf, size_t maxlen) {
    if (!running_) {
        throw std::runtime_error("Server not running");
    }
    
    if (buf == nullptr || maxlen == 0) {
        return 0;
    }

    ssize_t received = recv(client_fd_, buf, maxlen, 0);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0; // No data available
        }
        throw std::runtime_error("Receive failed: " + std::string(strerror(errno)));
    }
    if (received == 0) {
        close(client_fd_);
        client_fd_ = -1; // client disconnected
        return 0;
    }

    return received;
}