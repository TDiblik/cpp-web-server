#!/bin/bash

ulimit -n 65536

# Generate inline Lua scripts into the temporary directory
LUA_GET="/tmp/wrk-get.lua"
cat << 'EOF' > $LUA_GET
request = function()
  local random_size = math.random(10000, 60000)
  local header_content = string.rep("B", random_size)

  local headers = {}
  headers["X-Large-Header"] = header_content

  return wrk.format("GET", "/", headers, nil)
end
EOF

LUA_POST="/tmp/wrk-post.lua"
cat << 'EOF' > $LUA_POST
request = function()
  local random_body_size = math.random(1048576, 9437184)
  local body_content = string.rep("A", random_body_size)

  local headers = {}
  headers["Content-Type"] = "text/plain"

  if math.random(1, 10) > 8 then
    headers["X-Chaos-Header"] = string.rep("B", 40000)
  end

  return wrk.format("POST", "/lajse", headers, body_content)
end
EOF

# Wakes up the OS page cache, memory allocators, and thread pools before the real test begins.
echo "--- Warm-up ---"
wrk -t8 -c1000 -d5s http://localhost:8888/

echo "Waiting 2 seconds for sockets to clear..."
sleep 2

# Tests the kqueue event loop's ability to accept connections and multiplex tiny payloads
# without hitting TCP listen backlog drops or 100% CPU deadlocks.
echo -e "\n--- Baseline ---"
wrk -t8 -c10000 -d10s --timeout 5s http://localhost:8888/

echo "Waiting 2 seconds for sockets to clear..."
sleep 2

# The lua script generates an 'X-Large-Header' up to ~60KB. This forces the Request object to continuously
# trigger its resize_and_overwrite() logic and pushes right up against your HEADERS_MAX_SIZE (65536 bytes) limit.
echo -e "\n--- Buffer Allocation & Header Parsing Stress ---"
wrk -t4 -c5000 -d10s --timeout 5s -s $LUA_GET http://localhost:8888/

echo "Waiting 2 seconds for sockets to clear..."
sleep 2

# Generates huge request bodies (1MB to ~9.4MB), staying just under BODY_MAX_SIZE (10MB). It also has
# a 20% chance to inject a 40KB chaos header. Tests scatter/gather I/O handling of large memory blocks.
echo -e "\n--- Heavy Payloads & Fuzzing ---"
wrk -t4 -c100 -d15s --timeout 15s -s $LUA_POST http://localhost:8888/

# Clean up temporary Lua files
rm -f $LUA_GET $LUA_POST

echo -e "\n --- Complete ---"
