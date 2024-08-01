include(FetchContent)
find_package(Git REQUIRED)

FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
)
FetchContent_MakeAvailable(doctest)


file(GLOB SRC_FILES *.hpp *.cpp)
add_executable (SequentialThreadPool_test ${SRC_FILES})

target_include_directories(SequentialThreadPool_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(SequentialThreadPool_test PRIVATE doctest::doctest)

# ====================== CTest ======================
enable_testing()
include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(SequentialThreadPool_test)
