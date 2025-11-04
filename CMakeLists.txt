cmake_minimum_required(VERSION 3.5)

project(rwafi_contracts)

set(VERSION_MAJOR 0)
set(VERSION_MINOR 1)
set(VERSION_PATCH 0)
#set(VERSION_SUFFIX "alpha")

if(VERSION_SUFFIX)
  set(VERSION_FULL "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}-${VERSION_SUFFIX}")
else()
  set(VERSION_FULL "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")
endif()

message(STATUS "Updating version metadata, VERSION_FULL=${VERSION_FULL}")
add_custom_target(
   update_version ALL
   COMMAND ${CMAKE_COMMAND} -DVERSION_FULL=${VERSION_FULL}
                            -DGIT_REPOS_DIR=${CMAKE_SOURCE_DIR}
                            -DSRC_VERSION_FILE=${CMAKE_SOURCE_DIR}/version/contract_version.hpp.in
                            -DDEST_VERSION_FILE=${CMAKE_BINARY_DIR}/version/contract_version.hpp
                            -P ${CMAKE_SOURCE_DIR}/version/VersionUtils.cmake
   COMMENT "Updating version metadata..." VERBATIM )

message(STATUS "Building flon-system-contracts v${VERSION_FULL}")

include(ExternalProject)

find_package(flon.cdt)
message( STATUS "Found flon.cdt at \"${CDT_ROOT}\"")

option(SYSTEM_CONFIGURABLE_WASM_LIMITS
       "Enables use of the host functions activated by the CONFIGURABLE_WASM_LIMITS protocol feature" ON)

option(SYSTEM_BLOCKCHAIN_PARAMETERS
       "Enables use of the host functions activated by the BLOCKCHAIN_PARAMETERS protocol feature" ON)

option(SYSTEM_ENABLE_CHAIN_CORE_VERSION_CHECK
      "Enables a configure-time check that the version of chain core's tester library is compatible with this project's unit tests" ON)

option(SYSTEM_ENABLE_CDT_VERSION_CHECK
      "Enables a configure-time check that the version of CDT is compatible with this project's contracts" ON)

option(BUILD_TESTS "Build unit tests" OFF)

if(NOT "${CONTRACT_COMPILE_OPTIONS}" STREQUAL "")
  message(STATUS "Using CONTRACT_COMPILE_OPTIONS=${CONTRACT_COMPILE_OPTIONS}")
  set(CONTRACT_COMPILE_OPTIONS_FILE ${CMAKE_CURRENT_BINARY_DIR}/contracts/compile_options.txt)
  file(GENERATE OUTPUT ${CONTRACT_COMPILE_OPTIONS_FILE} CONTENT "${CONTRACT_COMPILE_OPTIONS}")
endif()

ExternalProject_Add(
  contracts_project
  SOURCE_DIR ${CMAKE_SOURCE_DIR}/contracts
  BINARY_DIR ${CMAKE_BINARY_DIR}/contracts
  INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/install
  CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release
             -DCMAKE_TOOLCHAIN_FILE=${CDT_TOOLCHAIN_FILE}
             -DSYSTEM_CONFIGURABLE_WASM_LIMITS=${SYSTEM_CONFIGURABLE_WASM_LIMITS}
             -DSYSTEM_BLOCKCHAIN_PARAMETERS=${SYSTEM_BLOCKCHAIN_PARAMETERS}
             -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
             -DBUILD_TESTS=${BUILD_TESTS}
             -DSYSTEM_ENABLE_CDT_VERSION_CHECK=${SYSTEM_ENABLE_CDT_VERSION_CHECK}
             -DCONTRACT_COMPILE_OPTIONS_FILE=${CONTRACT_COMPILE_OPTIONS_FILE}
  UPDATE_COMMAND ""
  PATCH_COMMAND ""
  TEST_COMMAND ""
  INSTALL_COMMAND ""
  BUILD_ALWAYS 1)

# Actually install subproject.
install(
  DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/install/
  DESTINATION "${CMAKE_INSTALL_PREFIX}"
  USE_SOURCE_PERMISSIONS # Remain permissions (rwx) for installed files
)

option(BUILD_TESTS "Build unit tests" OFF)

if(BUILD_TESTS)
  message(STATUS "Building unit tests.")
  add_subdirectory(tests)
else()
  message(STATUS "Unit tests will not be built. To build unit tests, set BUILD_TESTS to ON.")
endif()
