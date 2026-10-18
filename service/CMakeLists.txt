find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(PROTO_DIR ${CMAKE_SOURCE_DIR}/proto)
set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_DIR})

get_target_property(GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin LOCATION)
if(NOT GRPC_CPP_PLUGIN)
  find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin
    HINTS "$ENV{HOME}/.local/bin" /usr/local/bin)
endif()

set(out_pb_cc ${GENERATED_DIR}/raft.pb.cc)
set(out_grpc_cc ${GENERATED_DIR}/raft.grpc.pb.cc)
add_custom_command(
  OUTPUT ${out_pb_cc} ${GENERATED_DIR}/raft.pb.h
         ${out_grpc_cc} ${GENERATED_DIR}/raft.grpc.pb.h
  COMMAND protobuf::protoc
  ARGS --proto_path=${PROTO_DIR}
       --cpp_out=${GENERATED_DIR}
       --grpc_out=${GENERATED_DIR}
       --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
       ${PROTO_DIR}/raft.proto
  DEPENDS ${PROTO_DIR}/raft.proto
  COMMENT "protoc raft.proto"
  VERBATIM
)

add_library(vdb_raft_proto STATIC ${out_pb_cc} ${out_grpc_cc})
target_include_directories(vdb_raft_proto PUBLIC ${GENERATED_DIR})
target_link_libraries(vdb_raft_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)

add_library(vdb_service_lib STATIC
    src/index/DocStore.cc
    src/index/HnswSearcher.cc
    src/index/EmbedClient.cc
    src/raft/RaftLog.cc
    src/raft/RaftNode.cc
    src/sm/StateMachine.cc
    src/sm/QueryEngine.cc
    src/net/DataNodeClient.cc
    src/job/ClusterJob.cc
    src/api/HttpApi.cc
)
target_include_directories(vdb_service_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/libs/vector_index/third_party/hnswlib
)
target_link_libraries(vdb_service_lib PUBLIC
    exact_index_lib
    vdb_raft_proto
    vdb_tlv
)

add_executable(vectordb_service src/main_service.cc)
target_link_libraries(vectordb_service PRIVATE vdb_service_lib)

add_executable(raft_log_test tests/raft_log_test.cc)
target_link_libraries(raft_log_test PRIVATE vdb_service_lib)
