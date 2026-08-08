# Top-level CMakeLists for the DS18x20 sensor node firmware.
# Builds the `ds18x20_sensor` application from the main/ component.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ds18x20_sensor)