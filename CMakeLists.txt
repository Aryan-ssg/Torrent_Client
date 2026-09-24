# =============================================================================
# CMakeLists.txt - Build Configuration File
# =============================================================================
#
# This file tells CMake how to build your project.
# CMake is a build system (like Maven or Gradle in Java).
#
# JAVA COMPARISON:
# In Java with Maven, you have pom.xml:
#   <groupId>com.example</groupId>
#   <artifactId>torrent-client</artifactId>
#   <version>1.0</version>
#   <dependencies>...</dependencies>
#
# In C++ with CMake, you have CMakeLists.txt:
#   project(PeerFlow)
#   add_executable(peerflow src/main.cpp)
#   target_include_directories(...)
# =============================================================================

# Minimum CMake version required (like Maven version requirements)
cmake_minimum_required(VERSION 3.14)

# Project name and language (like <artifactId> and <packaging> in Maven)
# LANGUAGES CXX means we're using C++
project(PeerFlow LANGUAGES CXX)

# =============================================================================
# COMPILER SETTINGS
# =============================================================================

# Set C++ standard to C++17 (like Java 17 in pom.xml)
# C++17 gives us modern features like std::optional, std::string_view, etc.
set(CMAKE_CXX_STANDARD 17)

# Require C++17 (don't fall back to older standards)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Generate compile_commands.json (helps IDEs and tools understand your code)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# BUILD TARGET
# =============================================================================

# Create an executable (like <packaging>jar</packaging> in Maven)
# The executable will be named "peerflow" (or "peerflow.exe" on Windows)
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
find_package(Threads REQUIRED)

add_executable(peerflow
    src/main.cpp
    src/bencode/BencodeDecoder.cpp
    src/torrent/TorrentParser.cpp
    src/tracker/TrackerRequest.cpp
    src/tracker/HttpTracker.cpp
    src/net/TcpSocket.cpp
    src/peer/PeerHandshake.cpp
    src/peer/FakePeer.cpp
)

target_include_directories(peerflow PRIVATE include)
target_link_libraries(peerflow PRIVATE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

# =============================================================================
# HOW TO BUILD
# =============================================================================
#
# In terminal:
#   mkdir build
#   cd build
#   cmake ..
#   make
#
# Or use CMake directly:
#   cmake -B build
#   cmake --build build
#
# This creates:
#   build/peerflow  (the executable)
#   build/peerflow.exe  (on Windows)
#
# In Java terms:
#   mvn clean package  ->  cmake --build build
#   java -jar target/app.jar  ->  ./build/peerflow
# =============================================================================
