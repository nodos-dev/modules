# Copyright MediaZ Teknoloji A.S. All Rights Reserved.

cmake_minimum_required(VERSION 3.24.2)
project("Subsystems")

if (NOT WITH_NODOS_WORKSPACE)
    include(${SCRIPTS_DIR}/CMake/Subsystems.cmake)
endif()

# Update subsystems.json if you add dependencies, so that CD can create releases.

add_subdirectory(nosTestSubsystem)

add_subdirectory(nosAnimationSubsystem)
