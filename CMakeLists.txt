cmake_minimum_required(VERSION 3.23)
project(Chrono C)

set(CMAKE_C_STANDARD 99)

add_link_options(-lpthread -lrt)

add_executable(Chrono main.c logger.c logger.h)
