cmake_minimum_required(VERSION 3.20.0)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
cmake_language(DEFER CALL file CREATE_LINK
    ${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../compile_commands.json
    SYMBOLIC)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(test1)

target_sources(app PRIVATE src/main.c)
