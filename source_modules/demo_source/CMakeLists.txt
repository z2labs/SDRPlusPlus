cmake_minimum_required(VERSION 3.13)
project(demo_source)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(demo_source PRIVATE "src/" "../../misc_modules/sat_tracker/src")
