cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
    endif()
endif()

project(path_oc C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -nostdlib -std=gnu11")

add_executable(path_oc main.c)

target_link_libraries(path_oc
    taihenForKernel_stub
    taihenModuleUtils_stub
    SceThreadmgrForDriver_stub
)

vita_create_self(path_oc.skprx path_oc CONFIG exports.yml UNSAFE)
