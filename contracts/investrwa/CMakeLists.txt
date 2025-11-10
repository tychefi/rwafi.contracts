add_contract(invest.rwa invest.rwa ${CMAKE_CURRENT_SOURCE_DIR}/src/investrwa.cpp)

if(DEFINED ENV{DAY_SECONDS_FOR_TEST})
   message(WARNING "ENV{DAY_SECONDS_FOR_TEST}=$ENV{DAY_SECONDS_FOR_TEST} should use only for test!!!")
   target_compile_definitions(invest.rwa PUBLIC "DAY_SECONDS_FOR_TEST=$ENV{DAY_SECONDS_FOR_TEST}")
endif()


target_include_directories(invest.rwa
   PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(invest.rwa
   PROPERTIES
   RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")


target_compile_options( invest.rwa PUBLIC -R${CMAKE_CURRENT_SOURCE_DIR}/ricardian -R${CMAKE_CURRENT_BINARY_DIR}/ricardian )
target_link_libraries(invest.rwa flon_base)
