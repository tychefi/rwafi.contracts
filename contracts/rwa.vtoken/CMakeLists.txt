add_contract(rwa.vtoken rwa.vtoken ${CMAKE_CURRENT_SOURCE_DIR}/src/rwa.vtoken.cpp)

target_include_directories(rwa.vtoken
   PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(rwa.vtoken
   PROPERTIES
   RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/ricardian/rwa.vtoken.contracts.md.in ${CMAKE_CURRENT_BINARY_DIR}/ricardian/rwa.vtoken.contracts.md @ONLY )

target_compile_options( rwa.vtoken PUBLIC -R${CMAKE_CURRENT_SOURCE_DIR}/ricardian -R${CMAKE_CURRENT_BINARY_DIR}/ricardian )

install(
   FILES
      ${CMAKE_CURRENT_BINARY_DIR}/rwa.vtoken.wasm
      ${CMAKE_CURRENT_BINARY_DIR}/rwa.vtoken.abi
   DESTINATION
      ${CMAKE_INSTALL_PREFIX}/contracts/rwa.vtoken
)

target_link_libraries(rwa.vtoken flon_base)