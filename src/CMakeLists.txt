cmake_minimum_required(VERSION 3.12)
project(project1)

set(CMAKE_BUILE_TYPE DEBUG)
set(CMAKE_CXX_STANDARD 14)
SET(CMAKE_CXX_FLAGS_DEBUG "-g  -Wall")

AUX_SOURCE_DIRECTORY(./ DIR_SRCS)

include_directories(./)
include_directories(../../../linux-kernel/usr/include)

ADD_EXECUTABLE(exec ${DIR_SRCS})