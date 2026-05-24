# cpp web server (optimized)

This is an example of how to go from a totally unoptimized c++23 web server to a highly optimized version.

I mainly developed this as a learning project on how to optimize c++ code interating with sockets.

I'll present each version chronologically, starting from the intentionally naive implementation and ending with the most optimized version. For each step, I'll explain what changed, why it matters, and how it affected performance.

For compilation and (quick) testing of each version I used:
```sh
# Terminal 1
rm -rf build/
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/server

# Terminal 2
wrk -t2 -c400 -d10s http://localhost:8888/ # Mimics normal server requests
wrk -t2 -c400 -d10s -s scripts/wrk-get.lua http://localhost:8888/ # sends 64KB header
wrk -t2 -c400 -d10s -s scripts/wrk-post.lua http://localhost:8888/ # sends 64KB header and 10MB body
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

The main goal of these optimizations was to reduce syscalls as much as possible + add some allocation optimizations here and there. It can be found at commit `fb042f04c656a0c0ddf77b9a04b2aa1df24593ef`. To see all of the optimizations, run `git diff 072df00e03af5c9978e642f355cda08153a987a0 fb042f04c656a0c0ddf77b9a04b2aa1df24593ef`.

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
