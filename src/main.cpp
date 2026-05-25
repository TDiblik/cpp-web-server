#include "request/enums.hpp"
#include "request/request.hpp"
#include "server/server.hpp"

#include <csignal>
#include <cstdlib>
#include <print>
#include <thread>

void handle_sigint(int signum) { std::exit(signum); }
void listener();

int main() {
  std::signal(SIGINT, handle_sigint);
  std::signal(SIGPIPE, SIG_IGN);

  unsigned int num_threads = std::thread::hardware_concurrency();
  if (num_threads == 0) num_threads = 8;

  std::print("Starting server on {} hardware threads using SO_REUSEPORT...\n", num_threads);

  std::vector<std::thread> workers;
  workers.reserve(num_threads);
  for (unsigned int i = 0; i < num_threads; i++) workers.emplace_back(listener);
  for (auto& t : workers) t.join();

  return 0;
}

void listener() {
  Server server(8888);
  while (1) {
    int client_fd = server.accept();
    if (client_fd == -1) [[unlikely]] continue;

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
}
