cmake_minimum_required(VERSION 3.11)

project(proxy_battleground)

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_BUILD_TYPE Debug)

include(FetchContent)

FetchContent_Declare(
  wsserver_lib
  GIT_REPOSITORY https://github.com/Theldus/wsServer
  GIT_TAG        master
)

FetchContent_MakeAvailable(wsserver_lib)

add_executable(
    proxy_battleground
    
    ./src/main.c
    ./src/client_context.c
    )

target_link_libraries(proxy_battleground PRIVATE ws pthread)
target_include_directories(proxy_battleground PUBLIC include)
target_include_directories(proxy_battleground PRIVATE ${wsserver_lib_SOURCE_DIR}/include)