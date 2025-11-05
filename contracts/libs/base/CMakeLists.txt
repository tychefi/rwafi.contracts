file(GLOB_RECURSE SRC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c" "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE INC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")

add_library(flon_base STATIC ${SRC_FILES} ${INC_FILES})
target_include_directories(flon_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)