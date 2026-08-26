cmake_minimum_required(VERSION 3.16)
project(drive VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Td REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_package(Qt5 REQUIRED COMPONENTS Widgets Network)

set(SOURCES
    src/main.cpp
    src/core/backup_engine.cpp
    src/gui/main_window.cpp
    src/gui/auth_widget.cpp
)

set(HEADERS
    src/config.h
    src/core/backup_engine.h
    src/gui/main_window.h
    src/gui/auth_widget.h
)

add_executable(drive ${SOURCES} ${HEADERS})

target_include_directories(drive PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(drive PRIVATE
    Td::TdStatic
    SQLite::SQLite3
    Threads::Threads
    Qt5::Widgets
    Qt5::Network
)
