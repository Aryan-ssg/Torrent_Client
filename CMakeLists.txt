cmake_minimum_required(VERSION 3.14)
project(bittorrent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(bittorrent
    src/main.cpp
    src/bencode/BencodeDecoder.cpp
)

target_include_directories(bittorrent PRIVATE include)
