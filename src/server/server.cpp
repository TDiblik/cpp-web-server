#include "server.hpp"
#include "server_worker.hpp"

#if defined(__IS_LINUX__)
#include <pthread.h>
#endif

#include <thread>
#include <vector>

void Server::accept_and_handle() {
  unsigned int num_threads = std::thread::hardware_concurrency();
  if (num_threads == 0) num_threads = 8;

  std::vector<std::thread> workers;
  workers.reserve(num_threads);

  for (unsigned int i = 0; i < num_threads; i++) {
    workers.emplace_back([this, i]() {
      (void)i;
      #if defined(__IS_LINUX__)
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(i, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
      #endif

      ServerWorker worker(this->_port, this->_onHandled);
      worker.accept_and_handle();
    });
  }

  for (auto& t : workers) t.join();
}
