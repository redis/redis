cmake_minimum_required(VERSION 3.9)
set(CMAKE_BUILD_TYPE "Debug")
add_library(helloworld SHARED helloworld.c)
set_target_properties(helloworld PROPERTIES PREFIX "" SUFFIX ".so")


add_library(hellotype SHARED hellotype.c)
set_target_properties(hellotype PROPERTIES PREFIX "" SUFFIX ".so")


add_library(helloblock SHARED helloblock.c)
set_target_properties(helloblock PROPERTIES PREFIX "" SUFFIX ".so")


add_library(testmodule SHARED testmodule.c)
set_target_properties(testmodule PROPERTIES PREFIX "" SUFFIX ".so")