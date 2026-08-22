cmake_minimum_required(VERSION 3.15)
project(schwung-tapedelay VERSION 1.3.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
# No -ffast-math: it lets the compiler assume no NaN/Inf, which removes the
# safety of clamps on the realtime path (the ER-99 rule).
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

# Device runtime is glibc 2.35 / libstdc++ 6.0.29 — link the C++ runtime
# statically so the .so carries no libstdc++ version requirement at all.
set(TE2_STATIC_LINK -static-libstdc++ -static-libgcc)

set(TE2_INCLUDES
    src
    src/host
    src/ported/core
    src/ported/shared-dpf/dsp)

# The Move module: dlopen'd by Schwung's chain host as dsp.so
add_library(tape_echo2 SHARED
    src/dsp/tape_echo_plugin.cpp
    src/ported/core/TapeEchoDSP.cpp)
target_include_directories(tape_echo2 PRIVATE ${TE2_INCLUDES})
target_link_libraries(tape_echo2 PRIVATE m pthread)
target_link_options(tape_echo2 PRIVATE ${TE2_STATIC_LINK})
# audio_fx: the chain host hardcodes "<audio_fx>/<id>/<id>.so" and IGNORES the
# module.json "dsp" field (chain_host.c). Only sound_generators use dsp.so.
set_target_properties(tape_echo2 PROPERTIES OUTPUT_NAME "tapedelay" PREFIX "" SUFFIX ".so")

# Loader test: dlopens the real dsp.so exactly like the chain host does,
# renders audio through it, and times the realtime factor. Cross-compiled
# and run ON the Move — the go/no-go gate before wiring it into a set.
add_executable(te2_loadtest tools/loadtest.cpp)
target_include_directories(te2_loadtest PRIVATE ${TE2_INCLUDES})
target_link_libraries(te2_loadtest PRIVATE dl m)
target_link_options(te2_loadtest PRIVATE ${TE2_STATIC_LINK})

# Native DSP validation: golden checks on the vendored core + the sync math.
# Built and RUN in-container before cross-compiling.
add_executable(te2_test tests/test_dsp.cpp src/ported/core/TapeEchoDSP.cpp)
target_include_directories(te2_test PRIVATE ${TE2_INCLUDES})
target_link_libraries(te2_test PRIVATE m pthread)
target_link_options(te2_test PRIVATE ${TE2_STATIC_LINK})
