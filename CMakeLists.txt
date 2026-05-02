cmake_minimum_required(VERSION 3.16)

# Set the project name
set(PROJECT_NAME "smart_door_lock")

# Include ESP-IDF cmake functions
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Set the project
project(${PROJECT_NAME})