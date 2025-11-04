# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/richardchen/work/wasm.contracts/flon/rwafi.contracts/contracts")
  file(MAKE_DIRECTORY "/Users/richardchen/work/wasm.contracts/flon/rwafi.contracts/contracts")
endif()
file(MAKE_DIRECTORY
  "/Users/richardchen/work/wasm.contracts/flon/rwafi.contracts/build/contracts"
  "/Users/richardchen/work/wasm.contracts/flon/rwafi.contracts/build/install"
  "/Users/richardchen/work/wasm.contracts/flon/rwafi.contracts/build/contracts_project-prefix/tmp"
  "/Users/richardchen/work/wasm.contracts/flon/rwafi.contracts/build/contracts_project-prefix/src/contracts_project-stamp"
  "/Users/richardchen/work/wasm.contracts/flon/rwafi.contracts/build/contracts_project-prefix/src"
  "/Users/richardchen/work/wasm.contracts/flon/rwafi.contracts/build/contracts_project-prefix/src/contracts_project-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/richardchen/work/wasm.contracts/flon/rwafi.contracts/build/contracts_project-prefix/src/contracts_project-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/richardchen/work/wasm.contracts/flon/rwafi.contracts/build/contracts_project-prefix/src/contracts_project-stamp${cfgdir}") # cfgdir has leading slash
endif()
