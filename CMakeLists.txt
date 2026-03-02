cmake_minimum_required(VERSION 3.16)

# Version is injected by CI via -DPROJECT_VER=...
# Local builds without -DPROJECT_VER get "0.0.0-local"
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "0.0.0-local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bramble)
