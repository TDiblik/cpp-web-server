#pragma once

#include <cstdint>

enum HttpMethod : uint8_t {
  HTTP_GET,
  HTTP_HEAD,
  HTTP_POST,
  HTTP_PUT,
  HTTP_DELETE,
  HTTP_CONNECT,
  HTTP_OPTIONS,
  HTTP_TRACE,
  HTTP_PATCH,
};

enum RequestParseError : uint8_t {
  RequestParseError_Ok,
  RequestParseError_SocketError,
  RequestParseError_MalformedRequest,        // maps to: 400 Bad Request
  RequestParseError_HttpVersionNotSupported, // maps to: 505 HTTP Version Not Supported
  RequestParseError_PayloadTooLarge          // maps to: 413 Payload Too Large
};

enum ResponseCode : uint16_t {
  ResponseCode_OK = 200,

  ResponseCode_BadRequest = 400,
  ResponseCode_PayloadTooLarge = 413,

  ResponseCode_HttpVersionNotSupported = 505,
};
