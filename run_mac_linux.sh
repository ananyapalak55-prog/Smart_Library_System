#!/bin/bash
echo "Compiling Smart Library Network Edition for Mac/Linux..."
g++ -std=c++17 smart_library_network.cpp -o smart_library -pthread
if [ $? -ne 0 ]; then
    echo "[ERROR] Compilation failed! Make sure g++ is installed."
    exit 1
fi
echo "Compilation successful! Starting Smart Library System..."
echo ""
./smart_library
