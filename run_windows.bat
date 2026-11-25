@echo off
title Smart Library Management & Book Locator System
echo Compiling Smart Library Network Edition for Windows...
g++ -std=c++17 smart_library_network.cpp -o smart_library.exe -lws2_32
if %errorlevel% neq 0 (
    echo [ERROR] Compilation failed! Make sure MinGW g++ is installed and in PATH.
    pause
    exit /b %errorlevel%
)
echo Compilation successful! Starting Smart Library System...
echo.
smart_library.exe
pause
