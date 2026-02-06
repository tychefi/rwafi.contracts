add_contract(rwa.token rwa.token ${CMAKE_CURRENT_SOURCE_DIR}/src/rwa.token.cpp)

target_include_directories(rwa.token
   PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(rwa.token
   PROPERTIES
   RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/ricardian/rwa.token.contracts.md.in ${CMAKE_CURRENT_BINARY_DIR}/ricardian/rwa.token.contracts.md @ONLY )

target_compile_options( rwa.token PUBLIC -R${CMAKE_CURRENT_SOURCE_DIR}/ricardian -R${CMAKE_CURRENT_BINARY_DIR}/ricardian )

install(
   FILES
      ${CMAKE_CURRENT_BINARY_DIR}/rwa.token.wasm
      ${CMAKE_CURRENT_BINARY_DIR}/rwa.token.abi
   DESTINATION
      ${CMAKE_INSTALL_PREFIX}/contracts/rwa.token
)

target_link_libraries(rwa.token flon_base)