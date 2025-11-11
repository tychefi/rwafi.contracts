add_contract(rwafi.token rwafi.token ${CMAKE_CURRENT_SOURCE_DIR}/src/rwafi.token.cpp)

target_include_directories(rwafi.token
   PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(rwafi.token
   PROPERTIES
   RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/ricardian/rwafi.token.contracts.md.in ${CMAKE_CURRENT_BINARY_DIR}/ricardian/rwafi.token.contracts.md @ONLY )

target_compile_options( rwafi.token PUBLIC -R${CMAKE_CURRENT_SOURCE_DIR}/ricardian -R${CMAKE_CURRENT_BINARY_DIR}/ricardian )

install(
   FILES
      ${CMAKE_CURRENT_BINARY_DIR}/rwafi.token.wasm
      ${CMAKE_CURRENT_BINARY_DIR}/rwafi.token.abi
   DESTINATION
      ${CMAKE_INSTALL_PREFIX}/contracts/rwafi.token
)

target_link_libraries(rwafi.token flon_base)