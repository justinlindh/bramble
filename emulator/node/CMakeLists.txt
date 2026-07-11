# Bramble virtual node: the real firmware built for ESP-IDF's experimental
# POSIX/Linux simulator target.
#
#   source $IDF_PATH/export.sh
#   idf.py --preview set-target linux
#   idf.py build && ./build/bramble-node.elf
#
# The project reuses the firmware tree directly: the repo's main/ is this
# project's "main" component and the repo's components/ are the component
# search path. null_drivers/ holds spike-only no-op peripheral drivers
# (replaced by the emu_link virtual drivers in later tasks).
cmake_minimum_required(VERSION 3.16)

list(APPEND EXTRA_COMPONENT_DIRS
     "${CMAKE_CURRENT_LIST_DIR}/../../components"
     "${CMAKE_CURRENT_LIST_DIR}/../../main"
     "${CMAKE_CURRENT_LIST_DIR}/null_drivers")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bramble-node)
