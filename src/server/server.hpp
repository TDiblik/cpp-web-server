#pragma once

#include "server_worker.hpp"

#include <cstdint>
#include <sys/resource.h>

class Server {
  public:
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
    uint16_t _port;
    RequestHandler _onHandled;

  public:
    explicit Server(uint16_t port, RequestHandler onHandled) : _port(port), _onHandled(onHandled) {}
    ~Server() = default;

    void accept_and_handle();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
};
