cmake_minimum_required(VERSION 3.20)
project(Rift LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /utf-8 /EHsc /O2 /permissive-)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
endif()

include_directories(include)

file(GLOB_RECURSE SOURCES "src/*.cpp")
file(GLOB_RECURSE HEADERS "include/*.hpp")
set(RESOURCE_FILES "src/resource.rc" "src/app.manifest")

add_executable(rift ${SOURCES} ${HEADERS} ${RESOURCE_FILES})
target_link_libraries(rift PRIVATE ws2_32 wininet advapi32)

# Config Manager Tool
add_executable(configrif tools/configrif.cpp src/config.cpp src/socket_utils.cpp src/logger.cpp ${HEADERS})
target_link_libraries(configrif PRIVATE ws2_32 wininet advapi32)

# Copy config.json and create rft.exe alias in build output
add_custom_command(TARGET rift POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/config.json"
    $<TARGET_FILE_DIR:rift>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:rift>
    $<TARGET_FILE_DIR:rift>/rft.exe
)
