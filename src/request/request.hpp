#pragma once

#include "enums.hpp"

#include <string>
#include <unordered_map>

class Request {
  private:
    inline static constexpr uint32_t HEADERS_MAX_SIZE = 65536; // 64KB
    inline static constexpr uint32_t BODY_MAX_SIZE = 10485760; // 10MB

  private:
    int _client_fd;
    std::string _request_raw;
    std::string_view _headers_raw;

    void _client_fd_send(std::string_view message, int flags = 0);

  public:
    HttpMethod method;
    std::string_view path;
    std::string_view protocol;
    std::unordered_map<std::string_view, std::string_view> headers;
    std::string_view body;

    explicit Request(int client_fd);
    ~Request();

    // prevent copies, since it owns the file descriptor
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

    RequestParseError parse();
    void send_response(ResponseCode code, std::string_view content_type = "text/plain", std::string_view resp_body = {});
};
