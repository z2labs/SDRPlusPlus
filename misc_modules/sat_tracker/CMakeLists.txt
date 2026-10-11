cmake_minimum_required(VERSION 3.13)
project(sat_tracker)

file(GLOB SRC "src/*.cpp" "src/sgp4/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(sat_tracker PRIVATE "src/" "../../decoder_modules/radio/src")

if (MSVC)
    # windows.h (pulled in by core headers) must not define min/max macros
    target_compile_definitions(sat_tracker PRIVATE NOMINMAX _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
endif ()
