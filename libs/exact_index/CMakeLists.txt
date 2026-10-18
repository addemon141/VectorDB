cmake_minimum_required(VERSION 3.16)
project(exact_index LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(exact_index_lib STATIC
    src/ThreadPool.cc
    src/TextUtil.cc
    src/Deduper.cc
    src/ExactIndexBuilder.cc
    src/Bm25Index.cc
)
target_include_directories(exact_index_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    /usr/local/include
)
target_link_libraries(exact_index_lib PUBLIC tinyxml2 pthread)
