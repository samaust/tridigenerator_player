cmake_minimum_required(VERSION 3.10.2)

project(TDGenPlayer C CXX)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CTest)

if(ANDROID OR WIN32)
    add_subdirectory(dependencies/3rdParty)
    add_subdirectory(dependencies/meta/SampleXrFramework)
endif()
add_subdirectory(tridigenerator_player)
