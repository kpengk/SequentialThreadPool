cmake_minimum_required (VERSION 3.8)

project (SequentialThreadPool VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options("/utf-8")
    string(APPEND CMAKE_CXX_FLAGS " /Zc:__cplusplus")
endif ()

add_subdirectory(example)
add_subdirectory(test)
