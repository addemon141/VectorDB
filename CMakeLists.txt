cmake_minimum_required(VERSION 3.16)
project(VectorDB LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

list(PREPEND CMAKE_PREFIX_PATH "$ENV{HOME}/.local" "/usr/local")

add_subdirectory(libs/exact_index)
add_subdirectory(libs/vector_index)
add_subdirectory(common)
add_subdirectory(service)
add_subdirectory(datanode)

message(STATUS "VectorDB 配置完成。cmake --build . -j")
message(STATUS "可执行文件: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
