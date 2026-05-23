#include "request/enums.hpp"
#include "request/request.hpp"
#include "server/server.hpp"

#include <csignal>
#include <cstdlib>

void handle_sigint(int signum) { std::exit(signum); }
void handle_connection(int client_fd);

int main() {
  std::signal(SIGINT, handle_sigint);

  Server server(8888);
  while (1) {
    int client_fd = server.accept();
    if (client_fd == -1) [[unlikely]] continue;
    handle_connection(client_fd);
  }

  return 0;
}

void handle_connection(int client_fd) {
  Request req(client_fd);
  switch (req.parse()) {
    [[likely]]
    case RequestParseError_Ok: break;
    case RequestParseError_SocketError: return;
    case RequestParseError_HttpVersionNotSupported: req.send_response(ResponseCode_HttpVersionNotSupported);
    case RequestParseError_PayloadTooLarge: req.send_response(ResponseCode_PayloadTooLarge);
    case RequestParseError_MalformedRequest: req.send_response(ResponseCode_BadRequest);
  }

  req.send_response(ResponseCode_OK, "text/html", "<h1> Hello world! </h1>");
}
