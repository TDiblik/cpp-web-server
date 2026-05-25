#include "server.hpp"

#include <print>
#include <system_error>
#include <cstdint>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

Server::Server(uint16_t port, bool log_ip) : _socket_fd(-1), _port(port), _log_ip(log_ip) {
  this->_socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (this->_socket_fd == -1) throw std::system_error(errno, std::generic_category(), "socket creation failed");

  int opt = 1;

  int set_opt_result = setsockopt(this->_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  if (set_opt_result == -1) throw std::system_error(errno, std::generic_category(), "setting SO_REUSEADDR options failed");

  set_opt_result = ::setsockopt(this->_socket_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
  if (set_opt_result == -1) throw std::system_error(errno, std::generic_category(), "setting SO_REUSEPORT options failed");

  set_opt_result = ::setsockopt(this->_socket_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
  if (set_opt_result == -1) throw std::system_error(errno, std::generic_category(), "setting TCP_NODELAY failed");

  sockaddr_in server_addr = {
    .sin_family = AF_INET,
    .sin_port = htons(this->_port),
    .sin_addr = { .s_addr = INADDR_ANY }
  };
  int bind_result = bind(this->_socket_fd, (sockaddr*)&server_addr, sizeof(server_addr));
  if (bind_result == -1) throw std::system_error(errno, std::generic_category(), "binding the server socket failed");

  int listen_result = listen(this->_socket_fd, SOMAXCONN);
  if (listen_result == -1) throw std::system_error(errno, std::generic_category(), "listening on the server socket failed");
}

Server::~Server() {
  if (this->_socket_fd != -1) ::close(this->_socket_fd);
}

int Server::accept() {
  if (!this->_log_ip) [[likely]] return ::accept(this->_socket_fd, nullptr, nullptr);

  sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  int client_fd = ::accept(_socket_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
  if (client_fd == -1) [[unlikely]] return -1;

  char ip_str[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str)) != nullptr) [[likely]] {
    std::print("server accepted connection from: {}\n", ip_str);
  }

  return client_fd;
}
