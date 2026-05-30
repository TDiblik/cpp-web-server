#include "request.hpp"
#include "enums.hpp"

#include <charconv>
#include <cstddef>
#include <unistd.h>
#include <string>
#include <sys/socket.h>
#include <sys/uio.h>

Request::Request(int client_fd) :_client_fd(client_fd) {
  this->_request_raw.reserve(HEADERS_USUAL_SIZE);
  this->headers.reserve(USUAL_NUMBER_OF_HEADERS);
}

Request::~Request() {
  if (this->_client_fd != -1) [[likely]] ::close(this->_client_fd);
}

HeadersParseState Request::parse_headers() {
  if (this->_headers_parsing_state == HeadersParseState_Finished) return HeadersParseState_Finished;
  #define exit_fn(h) { this->_headers_parsing_state = h; return h; }

  // read req line + headers
  size_t current_size = this->_request_raw.size();
  size_t target_size = current_size + HEADERS_USUAL_SIZE;
  if (this->_request_raw.capacity() < target_size) {
    this->_request_raw.reserve(std::max(target_size, this->_request_raw.capacity() * 2));
  }

  ssize_t actual_bytes_read = 0;
  this->_request_raw.resize_and_overwrite(this->_request_raw.capacity(), [&](char* buf, size_t buf_capacity) {
    actual_bytes_read = ::read(this->_client_fd, buf + current_size, buf_capacity - current_size);
    if (actual_bytes_read <= 0) return current_size;
    return current_size + static_cast<size_t>(actual_bytes_read);
  });
  if (actual_bytes_read < 0) [[unlikely]] {
    if (errno == EAGAIN || errno == EWOULDBLOCK) exit_fn(HeadersParseState_NotFinished);
    exit_fn(HeadersParseState_SocketError);
  }
  if (actual_bytes_read == 0) [[unlikely]] exit_fn(HeadersParseState_ClientClosed);
  if (this->_request_raw.size() >= HEADERS_MAX_SIZE) [[unlikely]] exit_fn(HeadersParseState_TooLargeError);

  // Make sure req line is fully read
  if (this->_req_line_end == std::string::npos) {
    size_t req_search_start = (this->_req_line_scanned_pos >= 1) ? this->_req_line_scanned_pos - 1 : 0;
    this->_req_line_end = this->_request_raw.find("\r\n", req_search_start);
    if (this->_req_line_end == std::string::npos) [[unlikely]] {
      this->_req_line_scanned_pos = this->_request_raw.size();
      if (this->_request_raw.size() > REQ_LINE_MAX_LEN) [[unlikely]] exit_fn(HeadersParseState_MalformedRequest);
      exit_fn(HeadersParseState_NotFinished);
    }
  }

  // Make sure headers are fully read up until the end
  this->_headers_parsing_search_start = this->_req_line_end + 2;
  size_t search_start = (this->_headers_scanned_pos >= 3) ? this->_headers_scanned_pos - 3 : 0;
  this->_headers_parsing_search_end = this->_request_raw.find("\r\n\r\n", search_start);
  if (this->_headers_parsing_search_end == std::string::npos) [[unlikely]] {
    this->_headers_scanned_pos = this->_request_raw.size();
    if (this->_request_raw.size() >= HEADERS_MAX_SIZE) [[unlikely]] exit_fn(HeadersParseState_TooLargeError);
    exit_fn(HeadersParseState_NotFinished);
  }

  // parse request line (example: `GET /some/path HTTP/1.1`)
  {
    std::string_view req_line(this->_request_raw.data(), this->_req_line_end);
    size_t first_space = req_line.find(' ');
    size_t second_space = req_line.find(' ', first_space + 1);
    if (first_space == std::string::npos || second_space == std::string::npos) [[unlikely]] exit_fn(HeadersParseState_MalformedRequest);

    std::string_view method_str = req_line.substr(0, first_space);
    if (method_str.empty()) [[unlikely]] exit_fn(HeadersParseState_MalformedRequest);
    switch (method_str[0]) {
      case 'G': if (method_str == "GET") this->method = HTTP_GET; break;
      case 'P':
        if (method_str == "POST") this->method = HTTP_POST;
        else if (method_str == "PUT") this->method = HTTP_PUT;
        else if (method_str == "PATCH") this->method = HTTP_PATCH;
        break;
      case 'H': if (method_str == "HEAD") this->method = HTTP_HEAD; break;
      case 'D': if (method_str == "DELETE") this->method = HTTP_DELETE; break;
      case 'C': if (method_str == "CONNECT") this->method = HTTP_CONNECT; break;
      case 'O': if (method_str == "OPTIONS") this->method = HTTP_OPTIONS; break;
      case 'T': if (method_str == "TRACE") this->method = HTTP_TRACE; break;
    }
    if (this->method == HTTP_UNKNOWN) [[unlikely]] exit_fn(HeadersParseState_MalformedRequest);
    this->path = req_line.substr(first_space + 1, second_space - first_space - 1);
    this->protocol = req_line.substr(second_space + 1);
    if (this->protocol != "HTTP/1.1" && this->protocol != "HTTP/1.0") [[unlikely]] exit_fn(HeadersParseState_HttpVersionNotSupported);
  }

  // parse headers
  {
    size_t pos = this->_headers_parsing_search_start;
    if (pos > this->_headers_parsing_search_end) [[unlikely]] exit_fn(HeadersParseState_MalformedRequest);

    while (pos < this->_headers_parsing_search_end) {
      size_t eol = this->_request_raw.find("\r\n", pos);
      if (eol == std::string::npos) [[unlikely]] exit_fn(HeadersParseState_MalformedRequest);

      std::string_view line(_request_raw.data() + pos, eol - pos);

      size_t colon = line.find(':');
      if (colon == std::string::npos) [[unlikely]] exit_fn(HeadersParseState_MalformedRequest);

      std::string_view name = line.substr(0, colon);

      size_t val_start = line.find_first_not_of(" \t", colon + 1);
      std::string_view value = (val_start == std::string::npos) ? std::string_view{} : line.substr(val_start);

      this->_append_header({name, value});
      pos = eol + 2;
    }
  }

  auto conn_header = this->get_header_value("Connection");
  if ((
      conn_header.has_value() && (conn_header.value() == "close" || conn_header.value() == "Close")
    ) || (
      this->protocol == "HTTP/1.0" && (!conn_header.has_value() || conn_header.value() != "Keep-Alive")
    )
  ) {
    this->keep_alive = false;
  }
  exit_fn(HeadersParseState_Finished);

  #undef exit_fn
}

BodyParseState Request::parse_body() {
  if (this->_body_parsing_state == BodyParseState_Finished) return BodyParseState_Finished;
  #define exit_fn(b) { this->_body_parsing_state = b; return b; }

  if (this->content_length == std::string::npos) {
    auto it = this->get_header_value("Content-Length");
    if (!it.has_value()) {
      this->content_length = 0;
      exit_fn(BodyParseState_Finished);
    }

    auto val = it.value();
    auto [_, err] = std::from_chars(val.data(), val.data() + val.size(), this->content_length);
    if (err != std::errc()) [[unlikely]] exit_fn(BodyParseState_MalformedRequest);
    if (this->content_length == 0) [[unlikely]] exit_fn(BodyParseState_Finished);
    if (this->content_length > BODY_MAX_SIZE) [[unlikely]] exit_fn(BodyParseState_PayloadTooLarge);
  }

  if (this->_headers_parsing_search_end == std::string::npos) [[unlikely]] exit_fn(BodyParseState_MalformedRequest);
  this->_body_start = this->_headers_parsing_search_end + 4; // skip \r\n\r\n

  size_t body_already_read = this->_request_raw.size() - this->_body_start;
  if (body_already_read < this->content_length) {
    size_t bytes_remaining = this->content_length - body_already_read;
    size_t current_size = this->_request_raw.size();
    size_t target_size = current_size + bytes_remaining;

    if (this->_request_raw.capacity() < target_size) {
      this->_request_raw.reserve(std::max(target_size, this->_request_raw.capacity() * 2));
    }

    ssize_t actual_bytes_read = 0;
    this->_request_raw.resize_and_overwrite(this->_request_raw.capacity(), [&](char* buf, size_t buf_capacity) {
      size_t max_read = std::min(buf_capacity - current_size, bytes_remaining);
      actual_bytes_read = ::read(this->_client_fd, buf + current_size, max_read);
      if (actual_bytes_read <= 0) return current_size;
      return current_size + static_cast<size_t>(actual_bytes_read);
    });

    if (actual_bytes_read < 0) [[unlikely]] {
      if (errno == EAGAIN || errno == EWOULDBLOCK) exit_fn(BodyParseState_NotFinished);
      exit_fn(BodyParseState_SocketError);
    }
    if (actual_bytes_read == 0) [[unlikely]] exit_fn(BodyParseState_ClientClosed);

    body_already_read = this->_request_raw.size() - this->_body_start;
    if (body_already_read < this->content_length) exit_fn(BodyParseState_NotFinished);
  }

  this->body = std::string_view(this->_request_raw.data() + this->_body_start, this->content_length);
  exit_fn(BodyParseState_Finished);

  #undef exit_fn
}

std::optional<HeaderType> Request::_find_header_raw(HeaderNameType header_name) {
  for (const auto& header : this->headers) if (header.first == header_name) return header;
  return std::nullopt;
}

void Request::_append_header(HeaderType header) { this->headers.emplace_back(header); }

std::optional<HeaderValueType> Request::get_header_value(HeaderNameType header_name) {
  auto header_opt = this->_find_header_raw(header_name);
  if (!header_opt.has_value()) return std::nullopt;
  return header_opt->second;
}

void Request::send_response(ResponseCode code, std::string_view content_type, std::string_view resp_body) {
  if (this->write_state != ResponseWriteState_Idle) return;

  std::string_view status_line;
  switch (code) {
    case ResponseCode_OK: status_line = "200 OK"; break;
    case ResponseCode_BadRequest: status_line = "400 Bad Request"; break;
    case ResponseCode_PayloadTooLarge: status_line = "413 Payload Too Large"; break;
    case ResponseCode_HttpVersionNotSupported: status_line = "505 HTTP Version Not Supported"; break;
    default: status_line = "500 Internal Server Error"; break;
  }

  int header_len = std::snprintf(
    this->_response_header_buf, sizeof(this->_response_header_buf),
    "HTTP/1.1 %.*s\r\n"
    "Content-Type: %.*s\r\n"
    "Content-Length: %zu\r\n"
    "Connection: %s\r\n\r\n",
    static_cast<int>(status_line.size()), status_line.data(),
    static_cast<int>(content_type.size()), content_type.data(),
    resp_body.size(),
    this->keep_alive ? "keep-alive" : "close"
  );

  if (header_len < 0 || static_cast<size_t>(header_len) >= sizeof(this->_response_header_buf)) [[unlikely]] {
    this->write_state = ResponseWriteState_SocketError;
    return;
  }

  this->write_state = ResponseWriteState_NotFinished;

  this->_response_iovecs[0].iov_base = this->_response_header_buf;
  this->_response_iovecs[0].iov_len = static_cast<size_t>(header_len);

  if (!resp_body.empty()) {
    this->_response_iovecs[1].iov_base = const_cast<char*>(resp_body.data());
    this->_response_iovecs[1].iov_len = resp_body.size();
    this->_response_iovec_count = 2;
  } else this->_response_iovec_count = 1;

  this->resume_response();
}

ResponseWriteState Request::resume_response() {
  if (this->write_state != ResponseWriteState_NotFinished) return this->write_state;

  while (this->_response_iovec_count > 0) {
    ssize_t written = ::writev(this->_client_fd, this->_response_iovecs, this->_response_iovec_count);

    if (written < 0) [[unlikely]] {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return ResponseWriteState_NotFinished;
      this->write_state = ResponseWriteState_SocketError;
      return this->write_state;
    }
    if (written == 0) [[unlikely]] {
      this->write_state = ResponseWriteState_ClientClosed;
      return this->write_state;
    }

    size_t bytes_written = static_cast<size_t>(written);
    if (bytes_written >= this->_response_iovecs[0].iov_len) {
      bytes_written -= this->_response_iovecs[0].iov_len;

      if (this->_response_iovec_count == 2) {
        if (bytes_written >= this->_response_iovecs[1].iov_len) this->_response_iovec_count = 0;
        else {
          this->_response_iovecs[1].iov_base = static_cast<char*>(this->_response_iovecs[1].iov_base) + bytes_written;
          this->_response_iovecs[1].iov_len -= bytes_written;
          this->_response_iovecs[0] = this->_response_iovecs[1];
          this->_response_iovec_count = 1;
        }
      } else this->_response_iovec_count = 0;
    } else {
      this->_response_iovecs[0].iov_base = static_cast<char*>(this->_response_iovecs[0].iov_base) + bytes_written;
      this->_response_iovecs[0].iov_len -= bytes_written;
    }
  }

  this->write_state = ResponseWriteState_Finished;
  return this->write_state;
}

void Request::reset_state() {
  size_t consumed_bytes = 0;
  if (this->_headers_parsing_search_end != std::string::npos) {
    consumed_bytes = this->_headers_parsing_search_end + 4; // up to \r\n\r\n
    if (this->content_length != std::string::npos && this->content_length > 0) {
      consumed_bytes += this->content_length;
    }
  }

  if (consumed_bytes > 0 && consumed_bytes < this->_request_raw.size()) this->_request_raw.erase(0, consumed_bytes);
  else this->_request_raw.clear();

  this->headers.clear();
  this->_headers_parsing_state = HeadersParseState_NotFinished;
  this->_req_line_end = std::string::npos;
  this->_req_line_scanned_pos = 0;
  this->_headers_scanned_pos = 0;
  this->_headers_parsing_search_start = std::string::npos;
  this->_headers_parsing_search_end = std::string::npos;

  this->_body_parsing_state = BodyParseState_NotFinished;
  this->_body_start = std::string::npos;

  this->_response_iovec_count = 0;

  this->method = HTTP_UNKNOWN;
  this->path = {};
  this->protocol = {};
  this->body = {};
  this->write_state = ResponseWriteState_Idle;
  this->content_length = std::string::npos;
  this->keep_alive = true;
}
