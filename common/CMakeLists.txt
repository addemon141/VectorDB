option(VDB_IO_URING "Use io_uring for snapshot file IO" ON)

set(VDB_TLV_SRCS
    tlv/tlv.cc
    tlv/socket_io.cc
    tlv/hash_util.cc
    tlv/file_io.cc
)

add_library(vdb_tlv STATIC ${VDB_TLV_SRCS})
target_include_directories(vdb_tlv PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party
)
target_link_libraries(vdb_tlv PUBLIC pthread)

if(VDB_IO_URING)
  find_library(URING_LIB uring)
  if(URING_LIB)
    target_compile_definitions(vdb_tlv PUBLIC VDB_IO_URING=1)
    target_link_libraries(vdb_tlv PUBLIC ${URING_LIB})
    message(STATUS "VDB_IO_URING=ON (liburing)")
  else()
    message(WARNING "liburing not found; building without io_uring")
    target_compile_definitions(vdb_tlv PUBLIC VDB_IO_URING=0)
  endif()
else()
  target_compile_definitions(vdb_tlv PUBLIC VDB_IO_URING=0)
  message(STATUS "VDB_IO_URING=OFF")
endif()
