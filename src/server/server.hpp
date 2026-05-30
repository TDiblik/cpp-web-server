#pragma once

#include "request/request.hpp"

#include <cstdint>
#include <functional>
#include <sys/event.h>
#include <sys/resource.h>

class Server {
  public:
    using RequestHandler = std::function<void(Request* req)>;

  private:
    inline static constexpr int MAX_EVENTS = 256; // best compromise between L1 cache and minimizing syscalls
    inline static const size_t ULIMIT = []() -> size_t {
      const size_t default_fallback = 65536;
      struct rlimit limit;
      if (getrlimit(RLIMIT_NOFILE, &limit) == 0) {
        if (limit.rlim_cur == RLIM_INFINITY) return default_fallback;
        return limit.rlim_cur;
      }
      return default_fallback;
    }();

  private:
    int _socket_fd;
    uint16_t _port;
    RequestHandler _onHandled;
    int _kq_ident;
    std::vector<std::unique_ptr<Request>> _requests;

  public:
    explicit Server(uint16_t port, RequestHandler onHandled);
    ~Server();

    void accept_and_handle();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
};
