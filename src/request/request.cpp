#include "request.hpp"
#include "enums.hpp"

#include <unistd.h>
#include <string>
#include <sys/socket.h>


Request::Request(int client_fd) : _client_fd(client_fd) {}

Request::~Request() {
  if (this->_client_fd != -1) [[likely]] ::close(this->_client_fd);
}

RequestParseError Request::parse() {
  // ---------------------------------------------------------
  // 1. Read the Request Line 1 byte at a time
  // ---------------------------------------------------------
  char req_line[512] = {0};
  size_t req_line_len = 0;
  bool req_line_ok = false;

  while (req_line_len < sizeof(req_line) - 1) {
    char c;
    if (::read(this->_client_fd, &c, 1) <= 0) return RequestParseError_SocketError;

    req_line[req_line_len++] = c;
    this->_request_raw.push_back(c); // Append to backing store for string_views later

    if (req_line_len >= 2 && req_line[req_line_len - 2] == '\r' && req_line[req_line_len - 1] == '\n') {
      req_line[req_line_len] = '\0';
      req_line_ok = true;
      break;
    }
  }
  if (!req_line_ok) return RequestParseError_MalformedRequest;

  // ---------------------------------------------------------
  // 2. Parse Request Line with sscanf
  // ---------------------------------------------------------
  char method_buf[16] = {0};
  char path_buf[256] = {0};
  char protocol_buf[16] = {0};

  if (sscanf(req_line, "%15s %255s %15s", method_buf, path_buf, protocol_buf) != 3) return RequestParseError_MalformedRequest;

  if (strcmp(method_buf, "GET") == 0) this->method = HTTP_GET;
  else if (strcmp(method_buf, "HEAD") == 0) this->method = HTTP_HEAD;
  else if (strcmp(method_buf, "POST") == 0) this->method = HTTP_POST;
  else if (strcmp(method_buf, "PUT") == 0) this->method = HTTP_PUT;
  else if (strcmp(method_buf, "DELETE") == 0) this->method = HTTP_DELETE;
  else if (strcmp(method_buf, "CONNECT") == 0) this->method = HTTP_CONNECT;
  else if (strcmp(method_buf, "OPTIONS") == 0) this->method = HTTP_OPTIONS;
  else if (strcmp(method_buf, "TRACE") == 0) this->method = HTTP_TRACE;
  else if (strcmp(method_buf, "PATCH") == 0) this->method = HTTP_PATCH;
  else return RequestParseError_MalformedRequest;

  if (strcmp(protocol_buf, "HTTP/1.1") != 0 && strcmp(protocol_buf, "HTTP/1.0") != 0) return RequestParseError_MalformedRequest;

  size_t method_pos = this->_request_raw.find(method_buf);
  size_t path_pos = this->_request_raw.find(path_buf, method_pos + strlen(method_buf));
  size_t protocol_pos = this->_request_raw.find(protocol_buf, path_pos + strlen(path_buf));

  this->path = std::string_view(this->_request_raw.data() + path_pos, strlen(path_buf));
  this->protocol = std::string_view(this->_request_raw.data() + protocol_pos, strlen(protocol_buf));

  // ---------------------------------------------------------
  // 3. Read Headers 1 byte at a time
  // ---------------------------------------------------------
  bool headers_ok = false;
  size_t headers_start_idx = this->_request_raw.size();
  size_t current_headers_len = 0;

  while (current_headers_len < HEADERS_MAX_SIZE) {
    char c;
    if (::read(this->_client_fd, &c, 1) <= 0) return RequestParseError_SocketError;

    this->_request_raw.push_back(c);
    current_headers_len++;

    size_t total_len = this->_request_raw.size();
    if (total_len >= 4 &&
        this->_request_raw[total_len - 4] == '\r' &&
        this->_request_raw[total_len - 3] == '\n' &&
        this->_request_raw[total_len - 2] == '\r' &&
        this->_request_raw[total_len - 1] == '\n') {
      headers_ok = true;
      break;
    }
  }
  if (!headers_ok) return RequestParseError_PayloadTooLarge;

  // ---------------------------------------------------------
  // 4. Parse Headers with C-style pointer math
  // ---------------------------------------------------------
  char* header_start = this->_request_raw.data() + headers_start_idx;

  while (true) {
    char* end_of_line = strstr(header_start, "\r\n");
    if (!end_of_line || end_of_line == header_start) break;

    char* colon = strchr(header_start, ':');
    if (!colon || colon > end_of_line) return RequestParseError_MalformedRequest;

    size_t name_len = (unsigned long)(colon - header_start);
    std::string_view name(header_start, name_len);

    char* value_start = colon + 1;
    while (*value_start == ' ' && value_start < end_of_line) value_start++;

    size_t value_len = (unsigned long)(end_of_line - value_start);
    std::string_view value(value_start, value_len);

    this->headers[name] = value;
    header_start = end_of_line + 2;
  }

  // ---------------------------------------------------------
  // 5. Read Body with atoi and malloc
  // ---------------------------------------------------------
  auto it = this->headers.find("Content-Length");
  if (it == this->headers.end()) return RequestParseError_Ok;

  std::string content_len_str = std::string(it->second);
  int parsed_len = atoi(content_len_str.c_str());

  if (parsed_len < 0) return RequestParseError_MalformedRequest;
  if (parsed_len == 0) return RequestParseError_Ok;

  size_t content_len = static_cast<size_t>(parsed_len);
  if (content_len > BODY_MAX_SIZE) return RequestParseError_PayloadTooLarge;

  char* temp_body_buf = (char*)malloc(content_len + 1);
  if (!temp_body_buf) return RequestParseError_SocketError;

  size_t body_bytes_read = 0;
  while (body_bytes_read < content_len) {
    ssize_t bytes_read = ::read(this->_client_fd, temp_body_buf + body_bytes_read, content_len - body_bytes_read);
    if (bytes_read <= 0) {
      free(temp_body_buf);
      return RequestParseError_SocketError;
    }
    body_bytes_read += static_cast<size_t>(bytes_read);
  }
  temp_body_buf[body_bytes_read] = '\0';

  size_t body_start_idx = this->_request_raw.size();
  this->_request_raw.append(temp_body_buf, body_bytes_read);
  free(temp_body_buf);

  this->body = std::string_view(this->_request_raw.data() + body_start_idx, body_bytes_read);

  return RequestParseError_Ok;
}

void Request::_client_fd_send(std::string_view message, int flags) {
  flags |= MSG_NOSIGNAL;

  // ---------------------------------------------------------
  // Send exactly 1 byte per system call
  // ---------------------------------------------------------
  for (size_t i = 0; i < message.length(); i++) {
    char c = message[i];
    ssize_t sent = ::send(this->_client_fd, &c, 1, flags);
    if (sent <= 0) return;
  }
}

void Request::send_response(ResponseCode code, std::string_view content_type, std::string_view resp_body) {
  std::string status_line;
  switch (code) {
    case ResponseCode_OK: status_line = "200 OK"; break;
    case ResponseCode_BadRequest: status_line = "400 Bad Request"; break;
    case ResponseCode_PayloadTooLarge: status_line = "413 Payload Too Large"; break;
    case ResponseCode_HttpVersionNotSupported: status_line = "505 HTTP Version Not Supported"; break;
    default: status_line = "500 Internal Server Error"; break;
  }

  std::string response = "";
  response += "HTTP/1.1 ";
  response += status_line;
  response += "\r\n";

  response += "Content-Type: ";
  response += std::string(content_type);
  response += "\r\n";

  response += "Content-Length: ";
  response += std::to_string(resp_body.size());
  response += "\r\n";

  response += "Connection: close\r\n\r\n";

  if (!resp_body.empty()) response += std::string(resp_body);
  this->_client_fd_send(response, 0);
}
