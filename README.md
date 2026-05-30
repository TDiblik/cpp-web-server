# cpp web server (optimized)

This is an example of how to go from a totally unoptimized c++23 web server to a highly optimized version.

I mainly developed this as a learning project on how to optimize c++ code interating with sockets.

I'll present each version chronologically, starting from the intentionally naive implementation and ending with the most optimized version. For each step, I'll explain what changed, why it matters, and how it affected performance.

For compilation and (quick) testing of each version I used:
```sh
# Terminal 1 (Server)
dos2unix ./scripts/start_server.sh
chmod +x ./scripts/start_server.sh
./scripts/start_server.sh

# Terminal 2 (Stress Testing)
dos2unix ./scripts/stress_test.sh
chmod +x ./scripts/stress_test.sh
./scripts/stress_test.sh
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
--- Warm-up ---
Running 5s test @ http://localhost:8888/
  8 threads and 1000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    72.11ms    5.29ms  76.25ms   98.58%
    Req/Sec   221.79     47.80   330.00     66.00%
  8862 requests in 5.08s, 0.90MB read
Requests/sec:   1743.08
Transfer/sec:    180.45KB
Waiting 2 seconds for sockets to clear...

--- Baseline ---
Running 10s test @ http://localhost:8888/
  8 threads and 10000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   100.85ms   47.95ms 742.27ms   96.44%
    Req/Sec   162.41     51.49   330.00     72.34%
  12240 requests in 10.06s, 1.24MB read
  Socket errors: connect 0, read 9406, write 0, timeout 0
Requests/sec:   1217.24
Transfer/sec:    126.00KB
Waiting 2 seconds for sockets to clear...

--- Buffer Allocation & Header Parsing Stress ---
Running 10s test @ http://localhost:8888/
  4 threads and 5000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.23s   253.55ms   1.43s    90.80%
    Req/Sec    24.75     11.43    70.00     63.90%
  957 requests in 10.10s, 99.06KB read
  Socket errors: connect 0, read 4833, write 0, timeout 0
Requests/sec:     94.78
Transfer/sec:      9.81KB
Waiting 2 seconds for sockets to clear...

--- Heavy Payloads & Fuzzing ---
Running 15s test @ http://localhost:8888/
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   541.07ms   94.65ms 814.67ms   70.61%
    Req/Sec    45.23     14.96    90.00     65.20%
  2702 requests in 15.04s, 279.70KB read
Requests/sec:    179.64
Transfer/sec:     18.60KB

 --- Complete ---
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
--- Warm-up ---
Running 5s test @ http://localhost:8888/
  8 threads and 1000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    17.57ms    1.43ms  29.54ms   93.69%
    Req/Sec     0.90k   115.18     1.25k    70.75%
  35948 requests in 5.08s, 3.63MB read
Requests/sec:   7079.03
Transfer/sec:    732.79KB
Waiting 2 seconds for sockets to clear...

--- Baseline ---
Running 10s test @ http://localhost:8888/
  8 threads and 10000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    34.76ms   40.28ms 741.95ms   97.21%
    Req/Sec   470.78    198.43     1.01k    72.93%
  34627 requests in 10.10s, 3.50MB read
  Socket errors: connect 0, read 8363, write 0, timeout 0
Requests/sec:   3429.89
Transfer/sec:    355.05KB
Waiting 2 seconds for sockets to clear...

--- Buffer Allocation & Header Parsing Stress ---
Running 10s test @ http://localhost:8888/
  4 threads and 5000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    28.22ms   11.11ms 148.02ms   94.49%
    Req/Sec     1.12k   286.13     1.62k    79.38%
  43437 requests in 10.04s, 4.39MB read
  Socket errors: connect 0, read 4102, write 0, timeout 0
Requests/sec:   4328.17
Transfer/sec:    448.03KB
Waiting 2 seconds for sockets to clear...

--- Heavy Payloads & Fuzzing ---
Running 15s test @ http://localhost:8888/
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   287.08ms   32.24ms 375.03ms   68.26%
    Req/Sec    71.27     18.65   141.00     65.88%
  4244 requests in 15.05s, 439.32KB read
Requests/sec:    281.94
Transfer/sec:     29.19KB

 --- Complete ---
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
--- Warm-up ---
Running 5s test @ http://localhost:8888/
  8 threads and 1000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    16.57ms    1.85ms  33.91ms   94.28%
    Req/Sec     0.96k   136.06     1.36k    68.25%
  38083 requests in 5.07s, 3.85MB read
Requests/sec:   7506.97
Transfer/sec:    777.09KB
Waiting 2 seconds for sockets to clear...

--- Baseline ---
Running 10s test @ http://localhost:8888/
  8 threads and 10000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    30.90ms   36.95ms 851.45ms   97.53%
    Req/Sec   538.34    223.37     1.02k    70.90%
  39402 requests in 10.06s, 3.98MB read
  Socket errors: connect 0, read 8085, write 0, timeout 0
Requests/sec:   3915.14
Transfer/sec:    405.28KB
Waiting 2 seconds for sockets to clear...

--- Buffer Allocation & Header Parsing Stress ---
Running 10s test @ http://localhost:8888/
  4 threads and 5000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    25.80ms   11.76ms 193.87ms   95.50%
    Req/Sec     1.23k   313.98     1.74k    77.06%
  47584 requests in 10.09s, 4.81MB read
  Socket errors: connect 0, read 3997, write 0, timeout 0
Requests/sec:   4717.43
Transfer/sec:    488.33KB
Waiting 2 seconds for sockets to clear...

--- Heavy Payloads & Fuzzing ---
Running 15s test @ http://localhost:8888/
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   279.40ms   26.50ms 412.50ms   66.91%
    Req/Sec    72.86     17.92   130.00     75.47%
  4333 requests in 15.10s, 448.53KB read
Requests/sec:    286.98
Transfer/sec:     29.71KB

 --- Complete ---
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
--- Warm-up ---
Running 5s test @ http://localhost:8888/
  8 threads and 1000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    16.04ms    1.65ms  30.64ms   93.32%
    Req/Sec     0.99k   134.50     1.35k    64.75%
  39328 requests in 5.07s, 3.98MB read
Requests/sec:   7753.29
Transfer/sec:    802.59KB
Waiting 2 seconds for sockets to clear...

--- Baseline ---
Running 10s test @ http://localhost:8888/
  8 threads and 10000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    32.80ms   72.04ms   1.36s    98.62%
    Req/Sec   534.14    228.20     1.03k    71.86%
  37074 requests in 10.10s, 3.75MB read
  Socket errors: connect 0, read 8081, write 0, timeout 0
Requests/sec:   3669.76
Transfer/sec:    379.88KB
Waiting 2 seconds for sockets to clear...

--- Buffer Allocation & Header Parsing Stress ---
Running 10s test @ http://localhost:8888/
  4 threads and 5000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    26.35ms   10.72ms 136.02ms   93.97%
    Req/Sec     1.20k   323.93     1.91k    76.03%
  46537 requests in 10.04s, 4.70MB read
  Socket errors: connect 0, read 3951, write 0, timeout 0
Requests/sec:   4634.43
Transfer/sec:    479.74KB
Waiting 2 seconds for sockets to clear...

--- Heavy Payloads & Fuzzing ---
Running 15s test @ http://localhost:8888/
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   283.28ms   57.80ms 462.15ms   69.37%
    Req/Sec    67.40     16.92   111.00     64.30%
  4022 requests in 15.10s, 416.34KB read
Requests/sec:    266.33
Transfer/sec:     27.57KB

 --- Complete ---
```
