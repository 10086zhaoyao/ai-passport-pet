cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(FoloToy-AI-Passport)

# 本机 crosstool-NG 的 C++ 多架构头目录未进入默认搜索路径（Windows 安装问题），
# 按编译器安装位置探测并显式补入，否则 nvs_flash 等 C++ 组件会找不到 bits/c++config.h。
get_filename_component(_tc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
file(GLOB _tc_cxx "${_tc_bin}/../riscv32-esp-elf/include/c++/*/riscv32-esp-elf")
foreach(_d ${_tc_cxx})
    if(EXISTS "${_d}/rv32imc_zicsr_zifencei/ilp32/bits/c++config.h")
        idf_build_set_property(COMPILE_OPTIONS
            "-isystem${_d}/rv32imc_zicsr_zifencei/ilp32" APPEND)
    endif()
endforeach()
