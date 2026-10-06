# engine/CMakeLists.txt -- builds the C++ storage engine library, its tests,
# and the command-line tools.
#
# Targets:
#   epica_engine   static library: WAL, memtable, SSTables, compaction, DB
#   epica_net      static library: binary wire protocol, TCP server + client
#   epica_server   binary: serves a DB directory over TCP
#   epica_shell    binary: interactive local shell against a DB directory
#   wal_driver     binary: WAL crash-test driver (milestone 1)
#   db_driver      binary: whole-DB crash-test driver
#   engine_tests   GoogleTest suite for everything above

add_library(epica_engine STATIC
  src/crc32c.cc
  src/wal.cc
  src/write_batch.cc
  src/iterator.cc
  src/memtable.cc
  src/bloom.cc
  src/env.cc
  src/block.cc
  src/cache.cc
  src/table.cc
  src/merger.cc
  src/filenames.cc
  src/version.cc
  src/db_iter.cc
  src/db_impl.cc
)
target_include_directories(epica_engine
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(epica_engine PRIVATE -Wall -Wextra -Wpedantic -Werror)
find_package(Threads REQUIRED)
target_link_libraries(epica_engine PUBLIC Threads::Threads)

add_library(epica_net STATIC
  net/protocol.cc
  net/server.cc
  net/client.cc
)
target_include_directories(epica_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(epica_net PUBLIC epica_engine)
target_compile_options(epica_net PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(wal_driver tools/wal_driver.cc)
target_link_libraries(wal_driver PRIVATE epica_engine)

add_executable(db_driver tools/db_driver.cc)
target_link_libraries(db_driver PRIVATE epica_engine)

add_executable(epica_shell tools/epica_shell.cc)
target_link_libraries(epica_shell PRIVATE epica_net)

add_executable(epica_server tools/epica_server.cc)
target_link_libraries(epica_server PRIVATE epica_net)

if(EPICA_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(engine_tests
    tests/crc32c_test.cc
    tests/wal_test.cc
    tests/coding_test.cc
    tests/skiplist_test.cc
    tests/memtable_test.cc
    tests/write_batch_test.cc
    tests/bloom_test.cc
    tests/cache_test.cc
    tests/table_test.cc
    tests/version_test.cc
    tests/db_test.cc
    tests/net_test.cc
  )
  target_include_directories(engine_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(engine_tests PRIVATE epica_net epica_engine GTest::gtest_main)
  target_compile_options(engine_tests PRIVATE -Wall -Wextra)

  include(GoogleTest)
  gtest_discover_tests(engine_tests DISCOVERY_TIMEOUT 60)
endif()
