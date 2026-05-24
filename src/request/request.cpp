#include "request.hpp"
#include "enums.hpp"

#include <charconv>
#include <cstddef>
#include <unistd.h>
#include <string>
#include <sys/socket.h>
#include <sys/uio.h>

Request::Request(int client_fd) : _client_fd(client_fd), method(HTTP_UNKNOWN) {
  this->_request_raw.reserve(HEADERS_USUAL_SIZE);
  this->headers.reserve(USUAL_NUMBER_OF_HEADERS);
}

Request::~Request() {
  if (this->_client_fd != -1) [[likely]] ::close(this->_client_fd);
}

RequestParseError Request::parse() {
  // read req line + headers
  size_t headers_end = std::string::npos;
  {
    size_t search_start = 0;
    ssize_t bytes_read = 0;
    char buffer[HEADERS_USUAL_SIZE];
    while (true) {
      bytes_read = ::read(this->_client_fd, buffer, sizeof(buffer));
      if (bytes_read <= 0) [[unlikely]] return RequestParseError_SocketError;

      size_t bytes_read_t = static_cast<std::size_t>(bytes_read);
      if ((this->_request_raw.size() + bytes_read_t) >= HEADERS_MAX_SIZE) [[unlikely]] return RequestParseError_PayloadTooLarge;
      this->_request_raw.append(buffer, bytes_read_t);

      headers_end = this->_request_raw.find("\r\n\r\n", (search_start >= 3) ? search_start - 3 : 0); // -3 to catch split \r\n\r\n
      if (headers_end != std::string::npos) [[likely]] break; // likely since most requests are gonna fit into the HEADERS_USUAL_SIZE right away

      search_start = this->_request_raw.size();
    }
  }

  // parse request line (example: `GET /some/path HTTP/1.1`)
  size_t req_line_end = this->_request_raw.find("\r\n");
  if (req_line_end == std::string::npos) [[unlikely]] return RequestParseError_MalformedRequest;

  std::string_view req_line(this->_request_raw.data(), req_line_end);
  size_t first_space = req_line.find(' ');
  size_t second_space = req_line.find(' ', first_space + 1);
  if (first_space == std::string::npos || second_space == std::string::npos) [[unlikely]] return RequestParseError_MalformedRequest;

  std::string_view method_str = req_line.substr(0, first_space);
  if (method_str.empty()) [[unlikely]] return RequestParseError_MalformedRequest;
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
  if (this->method == HTTP_UNKNOWN) [[unlikely]] return RequestParseError_MalformedRequest;

  this->path = req_line.substr(first_space + 1, second_space - first_space - 1);

  this->protocol = req_line.substr(second_space + 1);
  if (this->protocol != "HTTP/1.1" && this->protocol != "HTTP/1.0") [[unlikely]] return RequestParseError_HttpVersionNotSupported;

  // parse headers
  size_t pos = req_line_end + 2; // skip the \r\n
  if (pos > headers_end) [[unlikely]] return RequestParseError_MalformedRequest;
  while (pos < headers_end) {
    // parse header line (example: `Connection: keep-alive`)
    size_t eol = this->_request_raw.find("\r\n", pos);
    if (eol == std::string::npos) [[unlikely]] return RequestParseError_MalformedRequest;

    std::string_view line(_request_raw.data() + pos, eol - pos);

    size_t colon = line.find(":");
    if (colon == std::string::npos) [[unlikely]] return RequestParseError_MalformedRequest;

    std::string_view name = line.substr(0, colon);

    size_t val_start = line.find_first_not_of(" \t", colon + 1);
    std::string_view value = (val_start == std::string::npos) ? std::string_view{} : line.substr(val_start);

    this->_append_header({name, value});
    pos = eol + 2;
  }

  // parse body
  size_t content_length = 0;
  {
    auto it = this->get_header_value("Content-Length");
    if (!it.has_value()) return RequestParseError_Ok;

    auto val = it.value();
    auto [_, err] = std::from_chars(val.data(), val.data() + val.size(), content_length);
    if (err != std::errc()) [[unlikely]] return RequestParseError_MalformedRequest;
  }
  if (content_length == 0) [[unlikely]] return RequestParseError_Ok;
  if (content_length > BODY_MAX_SIZE) [[unlikely]] return RequestParseError_PayloadTooLarge;

  // since we're reading HEADERS_USUAL_SIZE while reading headers, it's possible we've already read all of the body bytes
  // if not, calculate how many are left to read
  size_t body_start = headers_end + 4; // Skip past the \r\n\r\n
  size_t body_already_read = this->_request_raw.size() - body_start;
  if (body_already_read < content_length) {
    size_t bytes_remaining = content_length - body_already_read;
    size_t current_size = this->_request_raw.size();
    size_t new_size = current_size + bytes_remaining;
    this->_request_raw.resize_and_overwrite(new_size, [new_size](char*, size_t) { return new_size; }); // resize without zero-filling

    char* write_ptr = this->_request_raw.data() + current_size;
    while (bytes_remaining > 0) {
      ssize_t bytes_read = ::read(this->_client_fd, write_ptr, bytes_remaining);
      if (bytes_read <= 0) [[unlikely]] return RequestParseError_SocketError;

      write_ptr += bytes_read;
      bytes_remaining -= static_cast<std::size_t>(bytes_read);
    }
  }
  this->body = std::string_view(this->_request_raw.data() + body_start, content_length);

  return RequestParseError_Ok;
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
  std::string_view status_line;
  switch (code) {
    case ResponseCode_OK: status_line = "200 OK"; break;
    case ResponseCode_BadRequest: status_line = "400 Bad Request"; break;
    case ResponseCode_PayloadTooLarge: status_line = "413 Payload Too Large"; break;
    case ResponseCode_HttpVersionNotSupported: status_line = "505 HTTP Version Not Supported"; break;
    default: status_line = "500 Internal Server Error"; break;
  }

  char header_buf[256];
  int header_len = std::snprintf(
    header_buf, sizeof(header_buf),
    "HTTP/1.1 %.*s\r\n"
    "Content-Type: %.*s\r\n"
    "Content-Length: %zu\r\n"
    "Connection: close\r\n\r\n",
    static_cast<int>(status_line.size()), status_line.data(),
    static_cast<int>(content_type.size()), content_type.data(),
    resp_body.size()
  );

  if (header_len < 0 || static_cast<size_t>(header_len) >= sizeof(header_buf)) return;

  iovec iov[2];
  iov[0].iov_base = header_buf;
  iov[0].iov_len = static_cast<size_t>(header_len);
  int iovcnt = 1;

  if (!resp_body.empty()) {
    iov[1].iov_base = const_cast<char*>(resp_body.data());
    iov[1].iov_len = resp_body.size();
    iovcnt = 2;
  }

  int iov_index = 0;
  while (iov_index < iovcnt) {
    ssize_t written = ::writev(this->_client_fd, &iov[iov_index], iovcnt - iov_index);
    if (written <= 0) [[unlikely]] return;

    size_t bytes_to_advance = static_cast<size_t>(written);

    while (iov_index < iovcnt && bytes_to_advance > 0) {
      if (bytes_to_advance >= iov[iov_index].iov_len) {
        bytes_to_advance -= iov[iov_index].iov_len;
        iov_index++;
      } else {
        iov[iov_index].iov_base = static_cast<char*>(iov[iov_index].iov_base) + bytes_to_advance;
        iov[iov_index].iov_len -= bytes_to_advance;
        bytes_to_advance = 0;
      }
    }
  }
}
