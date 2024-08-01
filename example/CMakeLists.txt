file(GLOB SRC_FILES *.hpp *.cpp)
add_executable (SequentialThreadPool_example ${SRC_FILES})
target_include_directories(SequentialThreadPool_example PRIVATE ${CMAKE_SOURCE_DIR}/include)
