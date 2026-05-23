# cpp web server (optimized)

This is an example of how to go from a totally unoptimized c++23 web server to highly optimized version.

I mainly developed this as a learning project on how to optimize c++ code interating with sockets.

I'll present each version below, what optimizations I applied to it and how it affected the performance.

For compilation and (quick) testing of each version I used:
```sh
# Terminal 1
rm -rf build/
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/server

# Terminal 2
wrk -t2 -c400 -d10s http://localhost:8888/
wrk -t2 -c400 -d10s -s scripts/wrk-get.lua http://localhost:8888/
wrk -t2 -c400 -d10s -s scripts/wrk-post.lua http://localhost:8888/
```

Initial optimizations are significant enough that we don't need to measure it using professional tooling.

Final version of the code can be found at master, all of the other versions are refered to by their appropriate git tag.

## Naive version
I tried to write a version with as many beginner mistakes as possible. It can be found at commit <insert_after_commiting>.

Results:
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
