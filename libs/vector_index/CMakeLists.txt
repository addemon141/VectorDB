cmake_minimum_required(VERSION 3.16)
project(vector_index LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(vector_index_lib STATIC src/HnswBuilder.cc)
target_include_directories(vector_index_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/hnswlib
)
