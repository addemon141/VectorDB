add_executable(vectordb_datanode
    src/main_datanode.cc
    src/DataNodeServer.cc
    src/IndexBuilder.cc
)
target_include_directories(vectordb_datanode PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)
target_link_libraries(vectordb_datanode PRIVATE
    exact_index_lib
    vector_index_lib
    vdb_tlv
)
