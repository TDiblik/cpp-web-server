#pragma once

#include <cstdint>

class Server {
  private:
    int _socket_fd;
    uint16_t _port;
    bool _log_ip;

  public:
    explicit Server(uint16_t port, bool log_ip = false);
    ~Server();

    int accept();

    // prevent copies, since it owns the file descriptor
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
};
