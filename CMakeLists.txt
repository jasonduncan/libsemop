cmake_minimum_required(VERSION 3.24)
project(libsemop VERSION 0.1.0 LANGUAGES CXX)

option(LIBSEMOP_BUILD_EXAMPLES "Build the examples" ${PROJECT_IS_TOP_LEVEL})
option(LIBSEMOP_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
option(LIBSEMOP_INSTALL "Generate install rules and a CMake package" ${PROJECT_IS_TOP_LEVEL})
option(LIBSEMOP_WITH_TYPESAFE "Build the TypeSafe provider (needs libtypesafe)" ON)
option(LIBSEMOP_FETCH_DEPS "Fetch nlohmann_json and libtypesafe when find_package cannot find them" ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(FetchContent)

# The version in the public header must match the project version.
file(STRINGS include/semop/semop.hpp _version_line REGEX "std::string_view version = ")
string(REGEX MATCH "\"([0-9]+\\.[0-9]+\\.[0-9]+)\"" _ "${_version_line}")
if(NOT CMAKE_MATCH_1 STREQUAL PROJECT_VERSION)
  message(FATAL_ERROR "semop.hpp says version ${CMAKE_MATCH_1}; CMakeLists.txt says ${PROJECT_VERSION}")
endif()

# --- Dependencies -----------------------------------------------------------

find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
  if(NOT LIBSEMOP_FETCH_DEPS)
    message(FATAL_ERROR "nlohmann_json not found; install it or set LIBSEMOP_FETCH_DEPS=ON")
  endif()
  set(JSON_Install ${LIBSEMOP_INSTALL} CACHE BOOL "" FORCE)
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

if(LIBSEMOP_WITH_TYPESAFE)
  find_package(libtypesafe 0.1 QUIET)
  if(NOT libtypesafe_FOUND)
    if(NOT LIBSEMOP_FETCH_DEPS)
      message(FATAL_ERROR "libtypesafe not found; install it, set LIBSEMOP_FETCH_DEPS=ON, or LIBSEMOP_WITH_TYPESAFE=OFF")
    endif()
    # Develop against a local checkout with -DFETCHCONTENT_SOURCE_DIR_LIBTYPESAFE=<path>.
    set(LIBTYPESAFE_INSTALL ${LIBSEMOP_INSTALL} CACHE BOOL "" FORCE)
    FetchContent_Declare(libtypesafe
      URL https://github.com/jasonduncan/libtypesafe/archive/refs/tags/v0.1.0.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP ON)
    FetchContent_MakeAvailable(libtypesafe)
  endif()
endif()

# --- Compiler flags -----------------------------------------------------------

# Built without exceptions (it never throws) but with RTTI: callers built either way can link,
# subclass Provider, and use dynamic_cast on providers.
if(MSVC)
  set(_library_flags /W4 /permissive-)
  set(_strict_flags /EHs-c- /D_HAS_EXCEPTIONS=0 /GR-)
else()
  set(_library_flags -fno-exceptions -Wall -Wextra -Wpedantic)
  set(_strict_flags -fno-exceptions -fno-rtti)
endif()

# --- Core -------------------------------------------------------------------

add_library(libsemop STATIC
  src/answers.cpp
  src/bench.cpp
  src/filtering.cpp
  src/flows.cpp
  src/provider.cpp
  src/rerank.cpp
  src/util.cpp
  src/wire.cpp
  src/wrappers.cpp)
add_library(libsemop::semop ALIAS libsemop)
target_compile_features(libsemop PUBLIC cxx_std_17)
target_include_directories(libsemop PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(libsemop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(libsemop PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(libsemop PRIVATE ${_library_flags})
set_target_properties(libsemop PROPERTIES OUTPUT_NAME semop EXPORT_NAME semop VERSION ${PROJECT_VERSION})

# --- TypeSafe provider ------------------------------------------------------

if(LIBSEMOP_WITH_TYPESAFE)
  add_library(libsemop_typesafe STATIC src/typesafe.cpp)
  add_library(libsemop::typesafe ALIAS libsemop_typesafe)
  target_link_libraries(libsemop_typesafe PUBLIC libsemop libtypesafe::libtypesafe)
  target_include_directories(libsemop_typesafe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_options(libsemop_typesafe PRIVATE ${_library_flags})
  set_target_properties(libsemop_typesafe PROPERTIES
    OUTPUT_NAME semop_typesafe EXPORT_NAME typesafe VERSION ${PROJECT_VERSION})
endif()

# --- Examples ---------------------------------------------------------------

set(LIBSEMOP_EXAMPLES hello triage rerank filter ask)
if(LIBSEMOP_BUILD_EXAMPLES AND LIBSEMOP_WITH_TYPESAFE)
  foreach(example ${LIBSEMOP_EXAMPLES})
    add_executable(example_${example} examples/${example}.cpp)
    target_link_libraries(example_${example} PRIVATE libsemop::typesafe)
  endforeach()
  # The examples also compile as C++17 without exceptions or RTTI, as a strict host would.
  list(TRANSFORM LIBSEMOP_EXAMPLES PREPEND examples/ OUTPUT_VARIABLE _sources)
  list(TRANSFORM _sources APPEND .cpp)
  add_library(libsemop_api_check_strict OBJECT ${_sources})
  target_link_libraries(libsemop_api_check_strict PRIVATE libsemop::typesafe)
  target_compile_options(libsemop_api_check_strict PRIVATE ${_strict_flags})
endif()

# --- Install ------------------------------------------------------------------

if(LIBSEMOP_INSTALL)
  set(_config_dir ${CMAKE_INSTALL_LIBDIR}/cmake/libsemop)
  set(_targets libsemop)
  if(LIBSEMOP_WITH_TYPESAFE)
    list(APPEND _targets libsemop_typesafe)
  endif()
  install(TARGETS ${_targets} EXPORT libsemopTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY include/semop DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT libsemopTargets NAMESPACE libsemop:: DESTINATION ${_config_dir})
  configure_package_config_file(cmake/libsemopConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/libsemopConfig.cmake INSTALL_DESTINATION ${_config_dir})
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/libsemopConfigVersion.cmake
    COMPATIBILITY SameMinorVersion)
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/libsemopConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/libsemopConfigVersion.cmake
    DESTINATION ${_config_dir})
  install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/libsemop)
endif()

# --- Tests --------------------------------------------------------------------

if(LIBSEMOP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
