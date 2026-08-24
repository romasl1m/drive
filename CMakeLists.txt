cmake_minimum_required(VERSION 3.10)
project(drive CXX)

set(CMAKE_CXX_STANDARD 17)

find_package(Td REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

add_executable(drive src/main.cpp)
target_link_libraries(drive PRIVATE Td::TdStatic SQLite::SQLite3 Threads::Threads)
