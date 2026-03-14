cmake_minimum_required(VERSION 3.4...3.18)

# 1. Project Settings
project(taifex_resolver VERSION 1.0 LANGUAGES CXX)

# Specify C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find Threads (Required for std::thread in TAIFEX Parser)
find_package(Threads REQUIRED)

# 2. Build Target: Parser Object Library
# Using OBJECT library to avoid redundant compilation for static/shared targets
add_library(parser_obj OBJECT src/parser.cc)
target_include_directories(parser_obj PRIVATE include)
set_target_properties(parser_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)

# 3. Build Target: Static Library (libparser_static.a)
add_library(parser_static STATIC $<TARGET_OBJECTS:parser_obj>)
target_include_directories(parser_static PUBLIC include)
set_target_properties(parser_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

# 4. Build Target: C++ Interface Executable
# Maps to your: example/taifex_resolver_interface.cc
add_executable(taifex_resolver_cpp 
    example/taifex_resolver_interface.cc
)

# Include directories
target_include_directories(taifex_resolver_cpp PRIVATE include)

# Link Static Library and Threading Library
target_link_libraries(taifex_resolver_cpp 
    PRIVATE 
    parser_static 
    Threads::Threads
)

# 5. Output Configuration
# Set runtime output directory for the executable to ./build
set_target_properties(taifex_resolver_cpp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build"
)

# ----------------------------------------------------------------------------
# Post-Build: Copy static library to the ./build directory
# ----------------------------------------------------------------------------

add_custom_command(TARGET parser_static POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/build"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:parser_static> "${CMAKE_SOURCE_DIR}/build/"
    COMMENT "Copying parser_static library to ${CMAKE_SOURCE_DIR}/build/"
)
