#!/bin/bash

echo "Applying macOS Kernel Tuning..."
sudo sysctl -w kern.ipc.somaxconn=20000           # Expand TCP listen backlog
sudo sysctl -w kern.maxfiles=100000               # Raise global file limit
sudo sysctl -w kern.maxfilesperproc=100000        # Raise per-process file limit
sudo sysctl -w net.inet.ip.portrange.first=1024   # Prevent TIME_WAIT port exhaustion
sudo sysctl -w net.inet.ip.portrange.hifirst=1024

echo "Setting soft open-file ulimit..."
ulimit -n 65536

echo "Clearing build directory and old profiles..."
rm -rf build/
rm -f pgo.profraw pgo.profdata

echo "Building the server with PGO GENERATE..."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_PGO=GENERATE
cmake --build build -j

echo "Starting server in the background to capture PGO profile..."
LLVM_PROFILE_FILE="pgo.profraw" ./build/server &
SERVER_PID=$!

echo "Waiting 2 seconds for the server to bind..."
sleep 2

echo "Running stress test to generate profile data..."
./scripts/stress_test.sh

echo "Shutting down the server gracefully to flush profile data..."
kill -SIGINT $SERVER_PID
wait $SERVER_PID

echo "PGO profile generation complete!"

echo "Merging raw profile data into pgo.profdata..."
xcrun llvm-profdata merge -output=pgo.profdata pgo.profraw

echo "Merge complete! You can now build with PGO USE."
