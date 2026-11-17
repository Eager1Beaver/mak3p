cmake_minimum_required(VERSION 3.21)
project(PortableAudioToMP3 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AUDIO_CONVERTER_BUILD_GUI "Build the desktop application" ON)
option(AUDIO_CONVERTER_BUILD_AUDIO "Build audio processing and the command-line converter"
       ${AUDIO_CONVERTER_BUILD_GUI})

function(audio_converter_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

find_package(Threads REQUIRED)

add_library(audio_converter_core STATIC
    src/core/job.cpp
    src/core/path_utils.cpp
    src/core/output_naming.cpp
    src/core/output_transaction.cpp
)
target_include_directories(audio_converter_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(audio_converter_core PUBLIC cxx_std_20)
target_link_libraries(audio_converter_core PUBLIC Threads::Threads)
audio_converter_warnings(audio_converter_core)

if(AUDIO_CONVERTER_BUILD_GUI OR AUDIO_CONVERTER_BUILD_AUDIO)
    # vcpkg installs TagLib from the vcpkg.json manifest at configure time.
    find_package(TagLib 2.0 CONFIG REQUIRED)
endif()

if(AUDIO_CONVERTER_BUILD_AUDIO)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/locales/en-us.json")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/resources/locales/en-us.json" AUDIO_CONVERTER_EN_US)
    configure_file(src/audio/catalog.hpp.in generated/audio/catalog.hpp @ONLY)

    add_library(audio_converter_audio STATIC
        src/audio/process.cpp
        src/audio/diagnostic.cpp
        src/audio/metadata.cpp
        src/audio/audio.cpp
    )
    target_include_directories(audio_converter_audio PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_include_directories(audio_converter_audio SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
    target_link_libraries(audio_converter_audio PUBLIC audio_converter_core PRIVATE TagLib::tag)
    audio_converter_warnings(audio_converter_audio)

    add_executable(audio_converter_cli src/cli/main.cpp)
    target_link_libraries(audio_converter_cli PRIVATE audio_converter_audio)
    if(MINGW)
        target_link_options(audio_converter_cli PRIVATE -municode)
    endif()
    audio_converter_warnings(audio_converter_cli)
endif()

if(AUDIO_CONVERTER_BUILD_GUI)
    # Build the pinned FLTK release from source.
    include(FetchContent)
    set(FLTK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(FLTK_BUILD_TEST OFF CACHE BOOL "" FORCE)
    set(FLTK_BUILD_FLUID OFF CACHE BOOL "" FORCE)
    set(FLTK_BUILD_FLTK_OPTIONS OFF CACHE BOOL "" FORCE)
    set(FLTK_BUILD_GL OFF CACHE BOOL "" FORCE)
    set(FLTK_BUILD_FORMS OFF CACHE BOOL "" FORCE)
    set(FLTK_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

    # Use the X11 backend on Linux.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(FLTK_BACKEND_WAYLAND OFF CACHE BOOL "" FORCE)
        set(FLTK_BACKEND_X11 ON CACHE BOOL "" FORCE)
    endif()

    # Match the static MSVC CRT used by the x64-windows-static vcpkg triplet.
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
    target_link_libraries(portable_audio_to_mp3 PRIVATE
        audio_converter_core fltk::fltk TagLib::tag
    )

    audio_converter_warnings(portable_audio_to_mp3)
endif()

include(CTest)
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
