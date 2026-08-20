cmake_minimum_required(VERSION 3.10)
project(telegram_test CXX)

set(CMAKE_CXX_STANDARD 17)

find_package(Td REQUIRED)

add_executable(test test.cpp)
target_link_libraries(test PRIVATE Td::TdStatic)
