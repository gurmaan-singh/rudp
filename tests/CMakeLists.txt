include(FetchContent)
set(FETCHCONTENT_QUIET ON)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(rudp_tests
  test_packet.cpp
  test_rtt.cpp
  test_transfer.cpp
  test_udp.cpp)
target_link_libraries(rudp_tests PRIVATE rudp rudp_warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(rudp_tests DISCOVERY_TIMEOUT 60)
