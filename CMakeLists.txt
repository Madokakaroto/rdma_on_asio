cmake_minimum_required(VERSION 3.20)
project(rdma_on_asio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# NetworkDirect
set(ND_DIR ${CMAKE_SOURCE_DIR}/third_party/networkdirect/src/ndutil)
set(ND_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated/networkdirect)
file(MAKE_DIRECTORY ${ND_GENERATED_DIR})

find_program(MC_EXE mc.exe
    HINTS "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
)

add_custom_command(
    OUTPUT ${ND_GENERATED_DIR}/ndstatus.h
    COMMAND ${MC_EXE} -h ${ND_GENERATED_DIR} ${ND_DIR}/ndstatus.mc
    DEPENDS ${ND_DIR}/ndstatus.mc
    COMMENT "Generating ndstatus.h from ndstatus.mc"
)

add_library(ndutil STATIC
    ${ND_DIR}/ndaddr.cpp
    ${ND_DIR}/ndfrmwrk.cpp
    ${ND_DIR}/ndprov.cpp
    ${ND_GENERATED_DIR}/ndstatus.h
)
target_include_directories(ndutil PUBLIC ${ND_DIR} ${ND_GENERATED_DIR})
target_link_libraries(ndutil PUBLIC ws2_32)

# Main executable
add_executable(${PROJECT_NAME} src/main.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/asio/include
)

target_compile_definitions(${PROJECT_NAME} PRIVATE ASIO_STANDALONE)
target_link_libraries(${PROJECT_NAME} PRIVATE ndutil)
