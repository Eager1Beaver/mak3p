cmake_minimum_required(VERSION 3.21)
project(PortableAudioToMP3 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(TagLib 2.0 CONFIG REQUIRED)

include(FetchContent)
set(FLTK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FLUID OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FLTK_OPTIONS OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_GL OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FORMS OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(FLTK_BACKEND_WAYLAND OFF CACHE BOOL "" FORCE)
    set(FLTK_BACKEND_X11 ON CACHE BOOL "" FORCE)
endif()

if(MSVC)
    set(FLTK_MSVC_RUNTIME_DLL OFF CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(fltk
    GIT_REPOSITORY https://github.com/fltk/fltk.git
    GIT_TAG release-1.4.5
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(fltk)

add_executable(portable_audio_to_mp3 WIN32 src/main.cpp)
target_link_libraries(portable_audio_to_mp3 PRIVATE fltk::fltk TagLib::tag)

if(MSVC)
    target_compile_options(portable_audio_to_mp3 PRIVATE /W4)
else()
    target_compile_options(portable_audio_to_mp3 PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME application_version
        COMMAND portable_audio_to_mp3 --version)
endif()
