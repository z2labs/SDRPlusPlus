cmake_minimum_required(VERSION 3.13)
project(sat_tracker)

file(GLOB SRC "src/*.cpp" "src/sgp4/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(sat_tracker PRIVATE "src/" "../../decoder_modules/radio/src")
