# A separate project that uses libsemop the way a stranger would: installed (find_package) or
# vendored (add_subdirectory). Driven by tests/consumer/run.cmake.
cmake_minimum_required(VERSION 3.24)
project(libsemop_consumer LANGUAGES CXX)

if(LIBSEMOP_SOURCE_DIR)
  add_subdirectory(${LIBSEMOP_SOURCE_DIR} libsemop)
else()
  find_package(libsemop 0.1 REQUIRED)
endif()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE libsemop::typesafe)
target_compile_features(consumer PRIVATE cxx_std_17)

enable_testing()
add_test(NAME consumer COMMAND consumer)
