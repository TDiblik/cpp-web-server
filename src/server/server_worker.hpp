#pragma once

#include "request/request.hpp"

#include <vector>
#include <memory>
#include <functional>

using RequestHandler = std::function<void(Request* req)>;

class ServerWorker {
  friend class Server;

  private:
    inline static constexpr int MAX_EVENTS = 256; // best compromise between L1 cache and minimizing syscalls

    int _socket_fd;
    uint16_t _port;
    RequestHandler _onHandled;
    int _kq_ident;
    std::vector<std::unique_ptr<Request>> _requests;

  public:
    explicit ServerWorker(uint16_t port, RequestHandler onHandled);
    ~ServerWorker();

    void accept_and_handle();

    ServerWorker(const ServerWorker&) = delete;
    ServerWorker& operator=(const ServerWorker&) = delete;
};
