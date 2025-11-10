add_contract(yield.rwa yield.rwa ${CMAKE_CURRENT_SOURCE_DIR}/src/yieldrwa.cpp)

if(DEFINED ENV{DAY_SECONDS_FOR_TEST})
   message(WARNING "ENV{DAY_SECONDS_FOR_TEST}=$ENV{DAY_SECONDS_FOR_TEST} should use only for test!!!")
   target_compile_definitions(yield.rwa PUBLIC "DAY_SECONDS_FOR_TEST=$ENV{DAY_SECONDS_FOR_TEST}")
endif()


target_include_directories(yield.rwa
   PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(yield.rwa
   PROPERTIES
   RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")


target_compile_options( yield.rwa PUBLIC -R${CMAKE_CURRENT_SOURCE_DIR}/ricardian -R${CMAKE_CURRENT_BINARY_DIR}/ricardian )
target_link_libraries(yield.rwa flon_base)