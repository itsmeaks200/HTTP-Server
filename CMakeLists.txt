cmake_minimum_required(VERSION 3.16)
project(http_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra -Wpedantic)

add_executable(http_server
    src/main.cpp
    src/http/MimeTypes.cpp
    src/http/RequestParser.cpp
    src/http/Response.cpp
    src/http/Router.cpp
    src/util/StringUtils.cpp
)
target_include_directories(http_server PRIVATE include)
target_link_libraries(http_server PRIVATE Threads::Threads)
