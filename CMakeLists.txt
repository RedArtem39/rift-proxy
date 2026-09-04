cmake_minimum_required(VERSION 3.20)
project(ProxyClient LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /utf-8 /EHsc /O2 /permissive-)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
endif()

include_directories(include)

file(GLOB_RECURSE SOURCES "src/*.cpp")
file(GLOB_RECURSE HEADERS "include/*.hpp")

add_executable(proxy_client ${SOURCES} ${HEADERS})

target_link_libraries(proxy_client PRIVATE ws2_32 wininet)

# Copy config.json to build output
add_custom_command(TARGET proxy_client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/config.json"
    $<TARGET_FILE_DIR:proxy_client>
)
