cmake_minimum_required(VERSION 2.8)
project(neat_vi)

FILE(GLOB NEAT_VI_SRC ${CMAKE_CURRENT_LIST_DIR}/*.c)
add_executable(vi ${NEAT_VI_SRC})