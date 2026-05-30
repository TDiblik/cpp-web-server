# cpp web server (optimized)

This is an example of how to go from a totally unoptimized c++23 web server to a highly optimized version.

I mainly developed this as a learning project on how to optimize c++ code interating with sockets.

I'll present each version chronologically, starting from the intentionally naive implementation and ending with the most optimized version. For each step, I'll explain what changed, why it matters, and how it affected performance.

For compilation and (quick) testing of each version I used:
```sh
# Terminal 1 (Server)
ulimit -n 65536
rm -rf build/
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/server

# Terminal 2 (Stress Testing)
ulimit -n 65536

# Purpose: Tests the kqueue event loop's ability to accept connections and multiplex tiny payloads 
# without hitting TCP listen backlog drops or 100% CPU deadlocks.
wrk -t8 -c10000 -d10s --timeout 5s http://localhost:8888/ 

# Purpose: The lua script generates an 'X-Large-Header' up to ~60KB. This forces Request object to continuously trigger its resize_and_overwrite() logic and 
# pushes right up against your HEADERS_MAX_SIZE (65536 bytes) limit to test for OOMs or bounds errors.
wrk -t4 -c5000 -d10s --timeout 5s -s scripts/wrk-get.lua http://localhost:8888/ 

# Purpose: Generates huge request bodies (1MB to ~9.4MB), staying just under BODY_MAX_SIZE (10MB). It also has a 20% chance to inject a 40KB chaos header. 
# Tests the scatter/gather I/O handling of large memory blocks and the state machine's ability to transition cleanly from giant headers to giant bodies.
wrk -t4 -c100 -d15s --timeout 15s -s scripts/wrk-post.lua http://localhost:8888/
```

Initial optimizations are significant enough that we don't need to measure it using professional tooling.

Final version of the code can be found at master, all of the other versions are refered to by their appropriate git tag.

## Naive version
I tried to write a version with as many beginner mistakes as possible. It can be found at commit `072df00e03af5c9978e642f355cda08153a987a0`.

TLDR; 
- It reads the HTTP Request Line byte-by-byte (one `read` syscall per char).
- It uses `sscanf` to parse the request line (forces unnecessary memory copies).
- It pauses reading halfway to parse the request line, then starts a new read loop for the headers (ruins OS network buffering).
- It builds the outbound response using `+=` to concatenate everything. This thrashes the heap and doubles memory usage (serving a 10MB file takes 20MB of RAM).
- It double-copies the request body (reads into a temporary `malloc` buffer, then copies it into a `std::string`).
- It sends the response byte-by-byte (one `send` syscall per char, completely tanking throughput).
- It parses headers using unsafe, raw C pointer math (`strstr`, `strchr`).
- It allocates a brand new `std::string` just to pass the Content-Length view to `atoi()`.

### Results:
```sh
[cpp-web-server] (master) > wrk -t2 -c400 -d10s http://localhost:8888/
Running 10s test @ http://localhost:8888/
  2 threads and 400 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    81.65ms    4.49ms  94.26ms   94.66%
    Req/Sec   786.51     43.13     0.91k    68.50%
  15656 requests in 10.04s, 1.58MB read
  Socket errors: connect 151, read 91, write 0, timeout 0
Requests/sec:   1559.13
Transfer/sec:    161.40KB

[cpp-web-server] (master) > wrk -t2 -c400 -d10s -s scripts/wrk-get.lua http://localhost:8888/
Running 10s test @ http://localhost:8888/
  2 threads and 400 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.01s   582.17ms   1.99s    57.89%
    Req/Sec    29.98      5.37    40.00     72.36%
  602 requests in 10.10s, 62.32KB read
  Socket errors: connect 151, read 120, write 0, timeout 488
Requests/sec:     59.61
Transfer/sec:      6.17KB

[cpp-web-server] (master) > wrk -t2 -c400 -d10s -s scripts/wrk-post.lua http://localhost:8888/
Running 10s test @ http://localhost:8888/
  2 threads and 400 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.06s   547.55ms   1.98s    57.69%
    Req/Sec    20.73      6.85    50.00     77.01%
  398 requests in 10.10s, 41.20KB read
  Socket errors: connect 151, read 120, write 0, timeout 320
Requests/sec:     39.42
Transfer/sec:      4.08KB
```

## Optimize parsing

The main goal of these optimizations was to reduce syscalls as much as possible + add some allocation optimizations here and there. It can be found at commit `fb042f04c656a0c0ddf77b9a04b2aa1df24593ef`.

### `Request.cpp`

- Read headers and request line in a single `::read` call loop using a stack buffer.
```cpp
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

    headers_end = this->_request_raw.find("\r\n\r\n", (search_start >= 3) ? search_start - 3 : 0);
    if (headers_end != std::string::npos) [[likely]] break; 

    search_start = this->_request_raw.size();
  }
}
```
This ensures that we're not reading from the socket millions of times for a single request. By reading data in 4KB chunks, we drastically reduce context switches between user space and the kernel. It also includes an `O(1)` search resumption logic (search_start) so we don't rescan the entire string for `\r\n\r\n` on every loop iteration.

- Replace `sscanf` and raw C-pointer math with `std::string_view` math.
```cpp
// Request line parsing
size_t first_space = req_line.find(' ');
size_t second_space = req_line.find(' ', first_space + 1);
std::string_view method_str = req_line.substr(0, first_space);

// Header parsing
size_t colon = line.find(":");
std::string_view name = line.substr(0, colon);
size_t val_start = line.find_first_not_of(" \t", colon + 1);
```
Using `find` and `substr` on `string_view` creates zero runtime overhead and emits highly optimized assembly compared to `sscanf` (which copies memory) and manual pointer arithmetic (which is error-prone).

- Zero-allocation string-to-int conversion for the `Content-Length`.
```cpp
size_t content_length = 0;
auto [_, err] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), content_length);
```
Instead of converting the `string_view` into a `std::string` just to use `atoi()`, `std::from_chars` parses the integer directly from the pointer boundaries.

- Zero-copy Body Parsing.
```cpp
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
```
Instead of `malloc`-ing a temporary buffer and copying it into the C++ string, we calculate exactly how many bytes remain and use C++23's `resize_and_overwrite` to expand the string's capacity without zero-filling the memory. We then pass a pointer to `read()` to DMA the data directly into the heap buffer with absolute zero overhead, and simply bind a `std::string_view` to it.

- Eliminate the "God String" response builder.
```cpp
char header_buf[256];
int header_len = std::snprintf(
  header_buf, sizeof(header_buf),
  "HTTP/1.1 %.*s\r\n"
  "Content-Type: %.*s\r\n"
  "Content-Length: %zu\r\n"
  "Connection: close\r\n\r\n",
  // ... variables
);
this->_client_fd_send(std::string_view(header_buf, static_cast<size_t>(header_len)), 0);
if (!resp_body.empty()) this->_client_fd_send(resp_body, 0);
```
Instead of using `+=` to concatenate the headers and the body into one massive `std::string` (which forced the server to double its memory footprint just to serve a file), we write the headers into a lightweight stack buffer using `snprintf` and send the headers and body sequentially.

- Send responses in chunks, not byte-by-byte. The `_client_fd_send` method now uses a `while` loop that sends as much of the buffer as the socket will accept in a single system call, instead of artificially locking it to 1 byte per call.
```cpp
void Request::_client_fd_send(std::string_view message, int flags) {
  ssize_t sent = 0;
  size_t total_sent = 0;
  auto message_len = message.length();
  flags |= MSG_NOSIGNAL;

  while (total_sent < message_len) {
    sent = ::send(_client_fd, message.data() + total_sent, message_len - total_sent, flags);
    if (sent <= 0) [[unlikely]] return;
    total_sent += static_cast<size_t>(sent);
  }
}
```

### `Server.cpp`

- Disable Nagle's algorithm for lower HTTP latency.
```cpp
set_opt_result = ::setsockopt(this->_socket_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
if (set_opt_result == -1) throw std::system_error(errno, std::generic_category(), "setting TCP_NODELAY failed");
```
Forces the server to send data immediately instead of artificially delaying small packets to batch them together.

- Acceptation hot path optimization inside the Server::acept function:
```cpp
if (!this->_log_ip) [[likely]] return ::accept(this->_socket_fd, nullptr, nullptr);
```
Passing nullptr when IP logging is disabled saves CPU cycles by preventing an unnecessary kernel memory copy.

- Remove unnecessary initializations.
```cpp
sockaddr_in client_addr; // from sockaddr_in client_addr {};
// ...
char ip_str[INET_ADDRSTRLEN]; // from char ip_str[INET_ADDRSTRLEN] = {0};
// ...
```
The functions that assign values into them are going to rewrite them anyways.

### Results:
```sh
[cpp-web-server] (master) > wrk -t2 -c400 -d10s http://localhost:8888/
Running 10s test @ http://localhost:8888/
  2 threads and 400 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    15.31ms    2.61ms  52.98ms   95.07%
    Req/Sec     3.85k   288.60     4.25k    84.50%
  76528 requests in 10.04s, 7.74MB read
  Socket errors: connect 151, read 0, write 0, timeout 0
Requests/sec:   7622.41
Transfer/sec:    789.04KB

[cpp-web-server] (master) > wrk -t2 -c400 -d10s -s scripts/wrk-get.lua http://localhost:8888/
Running 10s test @ http://localhost:8888/
  2 threads and 400 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    16.55ms    2.33ms  34.63ms   82.80%
    Req/Sec     2.96k   187.33     3.46k    81.50%
  58932 requests in 10.04s, 5.96MB read
  Socket errors: connect 151, read 0, write 0, timeout 0
Requests/sec:   5871.20
Transfer/sec:    607.76KB

[cpp-web-server] (master) > wrk -t2 -c400 -d10s -s scripts/wrk-post.lua http://localhost:8888/
Running 10s test @ http://localhost:8888/
  2 threads and 400 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   142.90ms   43.44ms 210.26ms   78.86%
    Req/Sec   284.78    161.37   690.00     64.49%
  5195 requests in 10.05s, 16.25KB read
  Socket errors: connect 151, read 5033, write 1298, timeout 0
  Non-2xx or 3xx responses: 5099
Requests/sec:    517.00
Transfer/sec:      1.62KB
```

## Other flow micro-optimizations

The next set of optimizations focused on memory layout, data structures, and further syscall reduction. It can be found at commit `0df647a50f601d8bb49bea62152b827ac0a756bd`.

### `enums.hpp & request.hpp`

- Aligning the Struct Layout
```cpp
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
```
By enforcing explicit sizes on enums (`enum HttpMethod : uint8_t`) and adding `HTTP_UNKNOWN = 255`, the parser gets a cheap default state for detecting unsupported HTTP methods.

```cpp
class Request {
  // constants
  private:
    inline static constexpr uint32_t HEADERS_USUAL_SIZE = 4096; // 99% of headers will be this length
    inline static constexpr uint32_t HEADERS_MAX_SIZE = 65536; // 64KB
    inline static constexpr uint32_t USUAL_NUMBER_OF_HEADERS = 25;
    inline static constexpr uint32_t BODY_MAX_SIZE = 10485760; // 10MB

  // aligned members
  private:
    std::string _request_raw;
    std::string_view _headers_raw;
    int _client_fd;
  public:
    HttpMethod method;
    std::vector<HeaderType> headers;
    std::string_view path;
    std::string_view protocol;
    std::string_view body;
  // ...
};
```
By reordering the class members, we eliminate wasted padding. Placing the 4-byte `_client_fd` right next to the 1-byte `method` allows the compiler to pack them tightly into a single 8-byte boundary right before the 8-byte aligned `headers` vector begins. This shrinks the overall object size, reducing memory pressure and improving cache locality.

- Data-Oriented Design (Vector vs. Hash Map)
```cpp
// Replaced this:
std::unordered_map<std::string_view, std::string_view> headers;

// With this:
using HeaderNameType = std::string_view;
using HeaderValueType = std::string_view;
using HeaderType = std::pair<HeaderNameType, HeaderValueType>;

std::vector<HeaderType> headers;

// And in the constructor:
Request::Request(int client_fd) : _client_fd(client_fd), method(HTTP_UNKNOWN) {
  this->_request_raw.reserve(HEADERS_USUAL_SIZE);
  this->headers.reserve(USUAL_NUMBER_OF_HEADERS);
}
```
Swapping `std::unordered_map` for a `std::vector` of pairs is a performance win. For small collections (like 25 HTTP headers), the overhead of hashing a string, dealing with bucket collisions, and jumping around fragmented memory in a linked list is far slower than just doing a linear scan over a contiguous block of memory in a `std::vector`. Reserving the space in the constructor also eliminates allocations during parsing.

### `request.cpp`

- HTTP Method Switch Trick
```cpp
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
```
Replacing the massive if-else if string-comparison chain with a switch on the first character (`method_str[0]`) compiles into an optimized jump table. Since HTTP methods have conveniently unique starting letters, we instantly skip almost all the string comparisons.

- Gather I/O (`writev`)
```cpp
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
```
Replacing multiple `send()` calls with a single `writev()` using `iovec` avoids copying the header buffer and the body buffer into one giant string, and it drops system call overhead in half by sending both blocks of memory in a single kernel transition.

### Results:
```sh
[cpp-web-server] (master) > wrk -t2 -c400 -d10s http://localhost:8888/
Running 10s test @ http://localhost:8888/
  2 threads and 400 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    12.54ms    3.86ms  62.90ms   93.58%
    Req/Sec     4.30k   560.50     4.93k    86.87%
  85574 requests in 10.06s, 8.65MB read
  Socket errors: connect 151, read 0, write 0, timeout 0
Requests/sec:   8504.73
Transfer/sec:      0.86MB

[cpp-web-server] (master) > wrk -t2 -c400 -d10s -s scripts/wrk-get.lua http://localhost:8888/
Running 10s test @ http://localhost:8888/
  2 threads and 400 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    15.80ms    2.00ms  35.43ms   96.65%
    Req/Sec     3.36k   201.54     3.76k    79.50%
  66978 requests in 10.03s, 6.77MB read
  Socket errors: connect 151, read 0, write 0, timeout 0
Requests/sec:   6675.49
Transfer/sec:    691.02KB

[cpp-web-server] (master) > wrk -t2 -c400 -d10s -s scripts/wrk-post.lua http://localhost:8888/
Running 10s test @ http://localhost:8888/
  2 threads and 400 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   141.95ms   41.95ms 200.42ms   79.84%
    Req/Sec   263.54    155.84   666.00     67.24%
  3998 requests in 10.09s, 10.98KB read
  Socket errors: connect 151, read 3890, write 1334, timeout 0
  Non-2xx or 3xx responses: 3916
Requests/sec:    396.19
Transfer/sec:      1.09KB
```


## Multithreading
The next optimization was to stop running the whole server on a single thread and let the kernel distribute incoming connections between multiple listener sockets. It can be found at commit `4f8e4dc2c5264e49f7e2b1cbbdd63b862db8c2ce`.

### `CMakeLists.txt`

- Link pthreads
```cmake
set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

# ...

target_link_libraries(server ${CMAKE_THREAD_LIBS_INIT})
```
Since we're now using `std::thread`, we need to link the executable with the system threading library.

### `Server.cpp`

- Allow multi-threaded kernel load balancing
```cpp
set_opt_result = ::setsockopt(this->_socket_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
if (set_opt_result == -1) throw std::system_error(errno, std::generic_category(), "setting SO_REUSEPORT options failed");
```
`SO_REUSEPORT` allows multiple server sockets to bind to the same port. This lets each worker thread have its own listening socket, and the kernel can distribute incoming connections between them.

### `main.cpp`

- Spawn one listener per hardware thread
```cpp
unsigned int num_threads = std::thread::hardware_concurrency();
if (num_threads == 0) num_threads = 8;

std::print("Starting server on {} hardware threads using SO_REUSEPORT...\n", num_threads);

std::vector<std::thread> workers;
workers.reserve(num_threads);
for (unsigned int i = 0; i < num_threads; i++) workers.emplace_back(listener);
for (auto& t : workers) t.join();
```
Instead of running one server loop on the main thread, we now create one worker per hardware thread. Each worker runs its own `listener()` function, which creates its own `Server` instance and accepts connections independently.

- Ignore `SIGPIPE`
```cpp
std::signal(SIGPIPE, SIG_IGN);
```
When clients disconnect early, writing to the socket can trigger `SIGPIPE`. Since this is a normal thing under load testing, we ignore it and let the write path fail normally instead of killing the whole process.

### `request.cpp`

- Read headers directly into the request string
```cpp
size_t current_size = this->_request_raw.size();
ssize_t actual_bytes_read = 0;

this->_request_raw.resize_and_overwrite(current_size + HEADERS_USUAL_SIZE, [&](char* buf, size_t) {
  actual_bytes_read = ::read(this->_client_fd, buf + current_size, HEADERS_USUAL_SIZE);
  if (actual_bytes_read <= 0) return current_size;
  return current_size + static_cast<size_t>(actual_bytes_read);
});
if (actual_bytes_read <= 0) [[unlikely]] return RequestParseError_SocketError;

headers_end = this->_request_raw.find("\r\n\r\n", (search_start >= 3) ? search_start - 3 : 0);
if (headers_end != std::string::npos) [[likely]] break;
search_start = this->_request_raw.size();
```
The old version read into a stack buffer and then appended that buffer into `_request_raw`. This version uses `resize_and_overwrite` and reads directly into the final string storage.

### Results:
```sh
[cpp-web-server] (master) > wrk -t2 -c400 -d10s http://localhost:8888/
Running 10s test @ http://localhost:8888/
  2 threads and 400 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    11.54ms    3.28ms  41.19ms   82.84%
    Req/Sec     4.41k   336.64     5.05k    85.50%
  87774 requests in 10.04s, 8.87MB read
  Socket errors: connect 151, read 0, write 0, timeout 0
Requests/sec:   8744.03
Transfer/sec:      0.88MB

[cpp-web-server] (master) > wrk -t2 -c400 -d10s -s scripts/wrk-get.lua http://localhost:8888/
Running 10s test @ http://localhost:8888/
  2 threads and 400 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    16.30ms    2.73ms  52.87ms   92.90%
    Req/Sec     3.19k   230.22     3.50k    86.50%
  63543 requests in 10.05s, 6.42MB read
  Socket errors: connect 151, read 0, write 0, timeout 0
Requests/sec:   6323.34
Transfer/sec:    654.56KB

[cpp-web-server] (master) > wrk -t2 -c400 -d10s -s scripts/wrk-post.lua http://localhost:8888/
Running 10s test @ http://localhost:8888/
  2 threads and 400 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   874.63ms  122.41ms   1.06s    93.20%
    Req/Sec    71.42     19.56   141.00     75.00%
  1411 requests in 10.05s, 146.06KB read
  Socket errors: connect 151, read 112, write 0, timeout 0
Requests/sec:    140.38
Transfer/sec:     14.53KB
```
