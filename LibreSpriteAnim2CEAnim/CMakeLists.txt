cmake_minimum_required(VERSION 3.16)
project(LibreSpriteAnim2CEAnim)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)

add_executable(LibreSpriteAnim2CEAnim
    src/main.cpp
    src/tdf.cpp
)

target_compile_definitions(LibreSpriteAnim2CEAnim PRIVATE
    TDF_MODE_EXTERN
)