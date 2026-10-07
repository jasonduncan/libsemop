# Catch2 needs C++17 to print std::string_view in assertions.
set(CMAKE_CXX_STANDARD 17)
FetchContent_Declare(Catch2
  URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(Catch2)

# Offline: fake providers only.
add_executable(libsemop_tests
  test_answers.cpp
  test_provider.cpp
  test_wrappers.cpp
  test_flows.cpp
  test_filter_rerank.cpp
  test_bench.cpp
  test_wire.cpp)
target_link_libraries(libsemop_tests PRIVATE libsemop::semop Catch2::Catch2WithMain)
target_include_directories(libsemop_tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
include(Catch)
catch_discover_tests(libsemop_tests DISCOVERY_MODE PRE_TEST)

if(LIBSEMOP_WITH_TYPESAFE)
  # The TypeSafe provider against a scripted libtypesafe transport: offline.
  add_executable(libsemop_typesafe_tests test_typesafe.cpp)
  target_link_libraries(libsemop_typesafe_tests PRIVATE libsemop::typesafe Catch2::Catch2WithMain)
  catch_discover_tests(libsemop_typesafe_tests DISCOVERY_MODE PRE_TEST)

  # Live: calls the real API. Opt in with TYPESAFE_API_KEY=... LIBSEMOP_LIVE=1 ctest -L live
  add_executable(libsemop_live_tests test_live.cpp)
  target_link_libraries(libsemop_live_tests PRIVATE libsemop::typesafe Catch2::Catch2WithMain)
  add_test(NAME libsemop_live COMMAND libsemop_live_tests)
  set_tests_properties(libsemop_live PROPERTIES LABELS live SKIP_RETURN_CODE 4)
endif()

# The package works from a separate project: installed (find_package) and vendored (add_subdirectory).
if(LIBSEMOP_WITH_TYPESAFE)
  foreach(_mode install subdirectory)
    add_test(NAME consumer.${_mode}
      COMMAND ${CMAKE_COMMAND}
        -DMODE=${_mode}
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DBUILD_DIR=${PROJECT_BINARY_DIR}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/consumer-${_mode}
        -DCONFIG=$<CONFIG>
        "-DPREFIX_PATH=${CMAKE_PREFIX_PATH}"
        "-DTOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DGENERATOR=${CMAKE_GENERATOR}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/consumer/run.cmake)
    set_tests_properties(consumer.${_mode} PROPERTIES LABELS package TIMEOUT 900)
  endforeach()
endif()
