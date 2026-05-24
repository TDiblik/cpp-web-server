#pragma once

#include "enums.hpp"

#include <optional>
#include <string>
#include <vector>

using HeaderNameType = std::string_view;
using HeaderValueType = std::string_view;
using HeaderType = std::pair<HeaderNameType, HeaderValueType>;

class Request {
  // constants
  private:
    inline static constexpr uint32_t HEADERS_USUAL_SIZE = 4096; // 99% of headers will be this length
    inline static constexpr uint32_t HEADERS_MAX_SIZE = 65536; // 64KB
    inline static constexpr uint32_t USUAL_NUMBER_OF_HEADERS = 25;
    inline static constexpr uint32_t BODY_MAX_SIZE = 10485760; // 10MB

  // aligned members
  private:
    std::string _request_raw;
    std::string_view _headers_raw;
    int _client_fd;
  public:
    HttpMethod method;
    std::vector<HeaderType> headers;
    std::string_view path;
    std::string_view protocol;
    std::string_view body;

  // functions
  private:
    std::optional<HeaderType> _find_header_raw(HeaderNameType header_name);
    void _append_header(HeaderType header);

  public:
    explicit Request(int client_fd);
    ~Request();

    // prevent copies, since it owns the file descriptor
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

    RequestParseError parse();
    std::optional<HeaderValueType> get_header_value(HeaderNameType header_name);
    void send_response(ResponseCode code, std::string_view content_type = "text/plain", std::string_view resp_body = {});
};
