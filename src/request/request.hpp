#pragma once

#include "enums.hpp"

#include <optional>
#include <string>
#include <vector>
#include <sys/uio.h>

using HeaderNameType = std::string_view;
using HeaderValueType = std::string_view;
using HeaderType = std::pair<HeaderNameType, HeaderValueType>;

class Request {
  friend class Server;
  friend class ServerWorker;

  // constants
  private:
    inline static constexpr uint32_t REQ_LINE_MAX_LEN = 512; // 99% of headers will be this length
    inline static constexpr uint32_t HEADERS_USUAL_SIZE = 4096; // 99% of headers will be this length
    inline static constexpr uint32_t HEADERS_MAX_SIZE = 65536; // 64KB
    inline static constexpr uint32_t USUAL_NUMBER_OF_HEADERS = 25;
    inline static constexpr uint32_t BODY_MAX_SIZE = 10485760; // 10MB

  // aligned members
  private:
    std::string _request_raw;
    std::string_view _headers_raw;

    // IN-CLASS INITIALIZATION FOR DEFAULTS
    HeadersParseState _headers_parsing_state = HeadersParseState_NotFinished;
    size_t _req_line_end = std::string::npos;
    size_t _req_line_scanned_pos = 0;
    size_t _headers_scanned_pos = 0;
    size_t _headers_parsing_search_start = std::string::npos;
    size_t _headers_parsing_search_end = std::string::npos;

    BodyParseState _body_parsing_state = BodyParseState_NotFinished;
    size_t _body_start = std::string::npos;

    struct iovec _response_iovecs[2];
    int _response_iovec_count = 0;
    char _response_header_buf[256];

    int _client_fd;

  public:
    HttpMethod method = HTTP_UNKNOWN;
    std::vector<HeaderType> headers;
    std::string_view path;
    std::string_view protocol;
    std::string_view body;
    ResponseWriteState write_state = ResponseWriteState_Idle;
    size_t content_length = std::string::npos;
    bool keep_alive = true;



  // functions
  private:
    void _append_header(HeaderType header);
    std::optional<HeaderType> _find_header_raw(HeaderNameType header_name);

    HeadersParseState parse_headers();
    BodyParseState parse_body();
    ResponseWriteState resume_response();
    void reset_state();

  public:
    explicit Request(int client_fd);
    ~Request();

    // prevent copies, since it owns the file descriptor
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

    std::optional<HeaderValueType> get_header_value(HeaderNameType header_name);

    void send_response(ResponseCode code, std::string_view content_type = "text/plain", std::string_view resp_body = {});
};
