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
  HTTP_UNKNOWN = 255,
};

enum ResponseCode : uint16_t {
  ResponseCode_OK = 200,

  ResponseCode_BadRequest = 400,
  ResponseCode_PayloadTooLarge = 413,

  ResponseCode_HttpVersionNotSupported = 505,
};

enum HeadersParseState : uint8_t {
  HeadersParseState_NotFinished = 0,
  HeadersParseState_Finished = 1,

  HeadersParseState_SocketError = 10,
  HeadersParseState_ClientClosed = 11,
  HeadersParseState_TooLargeError = 12,
  HeadersParseState_MalformedRequest = 13,
  HeadersParseState_HttpVersionNotSupported = 14,
};

enum BodyParseState : uint8_t {
  BodyParseState_NotFinished = 0,
  BodyParseState_Finished = 1,

  BodyParseState_SocketError = 10,
  BodyParseState_ClientClosed = 11,
  BodyParseState_PayloadTooLarge = 12,
  BodyParseState_MalformedRequest = 13,
};

enum ResponseWriteState : uint8_t {
  ResponseWriteState_Idle = 0,
  ResponseWriteState_NotFinished = 1,
  ResponseWriteState_Finished = 2,

  ResponseWriteState_SocketError = 10,
  ResponseWriteState_ClientClosed = 11,
};
