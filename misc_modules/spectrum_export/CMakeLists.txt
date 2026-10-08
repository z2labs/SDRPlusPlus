cmake_minimum_required(VERSION 3.13)
project(spectrum_export)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})
