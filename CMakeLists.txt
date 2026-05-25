# south_park_td - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(south_park_td LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(SOUTHPARKTD_SOURCES
    src/main.cpp
    src/stubs.cpp
    src/launcher/launcher.cpp
    src/launcher/onboarding_dialog.cpp
)

if(WIN32)
    add_executable(south_park_td WIN32 ${SOUTHPARKTD_SOURCES})
else()
    add_executable(south_park_td ${SOUTHPARKTD_SOURCES})
endif()

rexglue_setup_target(south_park_td)
