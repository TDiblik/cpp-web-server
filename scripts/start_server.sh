#!/bin/bash

echo "Applying macOS Kernel Tuning..."
sudo sysctl -w kern.ipc.somaxconn=20000           # Expand TCP listen backlog
sudo sysctl -w kern.maxfiles=100000               # Raise global file limit
sudo sysctl -w kern.maxfilesperproc=100000        # Raise per-process file limit
sudo sysctl -w net.inet.ip.portrange.first=1024   # Prevent TIME_WAIT port exhaustion
sudo sysctl -w net.inet.ip.portrange.hifirst=1024

echo "Setting soft open-file ulimit..."
ulimit -n 65536

echo "Building the server..."
rm -rf build/
if [ -f "pgo.profdata" ]; then
  echo "Found pgo.profdata. Building with PGO USE..."
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_PGO=USE
else
  echo "No pgo.profdata found. Building without PGO..."
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_PGO=OFF
fi

cmake --build build -j

echo "Starting server..."
./build/server
