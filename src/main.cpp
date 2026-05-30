#include "request/enums.hpp"
#include "request/request.hpp"
#include "server/server.hpp"

#include <csignal>

void handle_sigint(int signum) { std::exit(signum); }

int main() {
  std::signal(SIGINT, handle_sigint);
  std::signal(SIGPIPE, SIG_IGN);


  Server server(8888, [](Request* req) {
    req->send_response(ResponseCode_OK, "text/html", "<h1> Hello world! </h1>");
  });
  server.accept_and_handle();

  return 0;
}
