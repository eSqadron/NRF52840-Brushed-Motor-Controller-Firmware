# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(MotorController)

target_include_directories(app PRIVATE
     "src" "include"
)

file(GLOB SRC_files
     "src/*.c"
)

file(GLOB INC_files
     "include/*.c"
     "include/**/*.c"
)

target_sources(app PRIVATE ${SRC_files} ${INC_files})

if ( CMAKE_COMPILER_IS_GNUCC )
     if ( CMAKE_C_COMPILER_VERSION LESS 13 )
          target_compile_options(app PRIVATE -Wno-unknown-pragmas)
     endif ()
endif ()
