cmake_minimum_required(VERSION 3.13)
project(demo_source)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(demo_source PRIVATE "src/" "../../misc_modules/sat_tracker/src")

if (MSVC)
    # windows.h (pulled in by core headers) must not define min/max macros
    target_compile_definitions(demo_source PRIVATE NOMINMAX _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
endif ()
