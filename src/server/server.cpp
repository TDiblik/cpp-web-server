#include "utils/sys.hpp"
#include "request/request.hpp"
#include "request/enums.hpp"
#include "server.hpp"

#include <sys/event.h>
#include <system_error>
#include <cstdint>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <fcntl.h>

Server::Server(uint16_t port, RequestHandler onHandled) : _socket_fd(-1), _port(port), _onHandled(onHandled), _kq_ident(-1) {
  this->_socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (this->_socket_fd == -1) throw std::system_error(errno, std::generic_category(), "socket creation failed");

  int opt = 1;

  int set_opt_result = setsockopt(this->_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  if (set_opt_result == -1) throw std::system_error(errno, std::generic_category(), "setting SO_REUSEADDR options failed");

  set_opt_result = ::setsockopt(this->_socket_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
  if (set_opt_result == -1) throw std::system_error(errno, std::generic_category(), "setting SO_REUSEPORT options failed");

  set_opt_result = ::setsockopt(this->_socket_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
  if (set_opt_result == -1) throw std::system_error(errno, std::generic_category(), "setting TCP_NODELAY failed");

  int flags = ::fcntl(this->_socket_fd, F_GETFL, 0);
  if (flags == -1) flags = 0;
  set_opt_result = ::fcntl(this->_socket_fd, F_SETFL, flags | O_NONBLOCK);
  if (set_opt_result == -1) throw std::system_error(errno, std::generic_category(), "setting flags | O_NONBLOCK failed");

  sockaddr_in server_addr = {
    #ifdef __IS_BSD__
        .sin_len = sizeof(sockaddr_in),
    #endif
    .sin_family = AF_INET,
    .sin_port = htons(this->_port),
    .sin_addr = { .s_addr = INADDR_ANY },
    .sin_zero = {0}
  };
  int bind_result = ::bind(this->_socket_fd, (sockaddr*)&server_addr, sizeof(server_addr));
  if (bind_result == -1) throw std::system_error(errno, std::generic_category(), "binding the server socket failed");

  int listen_result = ::listen(this->_socket_fd, SOMAXCONN);
  if (listen_result == -1) throw std::system_error(errno, std::generic_category(), "listening on the server socket failed");

  this->_kq_ident = kqueue();
  if (this->_kq_ident < 0) throw std::system_error(-1, std::generic_category(), "creating kqueue failed");

  struct kevent change_event;
  EV_SET(&change_event, this->_socket_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
  int kevent_result = kevent(this->_kq_ident, &change_event, 1, NULL, 0, NULL);
  if (kevent_result < 0) throw std::system_error(-1, std::generic_category(), "kevent register failed");

  this->_requests.resize(this->ULIMIT);
}

Server::~Server() {
  if (this->_socket_fd != -1) ::close(this->_socket_fd);
}

void Server::accept_and_handle() {
  struct kevent event_list[MAX_EVENTS];

  while (true) {
    const int num_of_new_events = kevent(this->_kq_ident, NULL, 0, event_list, MAX_EVENTS, NULL);
    for (int i = 0; i < num_of_new_events; i++) {
      auto event = event_list[i];
      const uintptr_t current_fd = event.ident;
      const int current_fd_i = static_cast<int>(current_fd);

      if (current_fd_i == this->_socket_fd) {
        while (true) {
          // todo: replace with accept4 on linux
          int client_fd = ::accept(this->_socket_fd, nullptr, nullptr);
          if (client_fd == -1) break;

          int set_opt_result = ::fcntl(client_fd, F_SETFL, O_NONBLOCK);
          if (set_opt_result == -1) { ::close(client_fd); continue; }

          struct kevent change_event;
          EV_SET(&change_event, client_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
          int kevent_result = kevent(this->_kq_ident, &change_event, 1, NULL, 0, NULL);
          if (kevent_result < 0) { ::close(client_fd); continue; }

          auto& req = this->_requests[static_cast<size_t>(client_fd)];
          if (req) {
            req->_client_fd = client_fd;
            req->reset_state();
          } else {
            req = std::make_unique<Request>(client_fd);
          }
        }
        continue;
      }

      Request* current_request = nullptr;
      if (current_fd < this->_requests.size() && this->_requests[current_fd]) {
        current_request = this->_requests[current_fd].get();
        if (current_request->_client_fd != current_fd_i) current_request = nullptr;
      }

      #define close_and_continue() { \
        if (current_request) { \
          ::close(current_request->_client_fd); \
          current_request->_client_fd = -1; \
        } \
        else ::close(current_fd_i); \
        continue; \
      }

      if (current_request == nullptr) [[unlikely]] close_and_continue();
      if (event.flags & EV_EOF && current_request->write_state == ResponseWriteState_Idle && current_request->_request_raw.empty()) {
        close_and_continue();
      }

      // State Machine Loop for HTTP Pipelining
      bool try_read = (event.filter == EVFILT_READ);
      bool try_write = (event.filter == EVFILT_WRITE);
      bool process_pipeline = true;
      while (process_pipeline) {
        process_pipeline = false;
        bool skip_to_write = false;

        // --- READ PHASE ---
        if (std::exchange(try_read, false) || !current_request->_request_raw.empty()) {
          switch (current_request->parse_headers()) {
            case HeadersParseState_NotFinished: skip_to_write = true; break;
            case HeadersParseState_Finished: break;
            case HeadersParseState_MalformedRequest: current_request->send_response(ResponseCode_BadRequest); break;
            case HeadersParseState_TooLargeError: current_request->send_response(ResponseCode_PayloadTooLarge); break;
            case HeadersParseState_HttpVersionNotSupported: current_request->send_response(ResponseCode_HttpVersionNotSupported); break;

            case HeadersParseState_SocketError:
            case HeadersParseState_ClientClosed: close_and_continue();
          }

          if (!skip_to_write && current_request->write_state == ResponseWriteState_Idle) {
            switch (current_request->parse_body()) {
              case BodyParseState_NotFinished: skip_to_write = true; break;
              case BodyParseState_Finished: break;
              case BodyParseState_PayloadTooLarge: current_request->send_response(ResponseCode_PayloadTooLarge); break;
              case BodyParseState_MalformedRequest: current_request->send_response(ResponseCode_BadRequest); break;

              case BodyParseState_SocketError:
              case BodyParseState_ClientClosed: close_and_continue();
            }
          }

          if (!skip_to_write && current_request->write_state == ResponseWriteState_Idle) this->_onHandled(current_request);

          if (current_request->write_state == ResponseWriteState_NotFinished) {
            struct kevent changes[2];
            EV_SET(&changes[0], current_fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
            EV_SET(&changes[1], current_fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
            kevent(this->_kq_ident, changes, 2, NULL, 0, NULL);
          }
        }

        // --- WRITE PHASE ---
        if (!(std::exchange(try_write, false) || current_request->write_state == ResponseWriteState_Finished) || current_request->write_state == ResponseWriteState_Idle) continue;
        switch (current_request->resume_response()) {
          case ResponseWriteState_Finished:
            if (!current_request->keep_alive) close_and_continue();
            if (event.filter == EVFILT_WRITE) {
              struct kevent changes[2];
              EV_SET(&changes[0], current_fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
              EV_SET(&changes[1], current_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
              kevent(this->_kq_ident, changes, 2, NULL, 0, NULL);
            }
            current_request->reset_state();
            if (!current_request->_request_raw.empty()) process_pipeline = true;
            break;

          case ResponseWriteState_NotFinished:
          case ResponseWriteState_Idle: break;

          case ResponseWriteState_SocketError:
          case ResponseWriteState_ClientClosed: close_and_continue();
        }

      } // End of process_pipeline loop
      #undef close_and_continue
    }
  }
}
