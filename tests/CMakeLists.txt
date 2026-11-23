include_directories("${CMAKE_CURRENT_SOURCE_DIR}/support")

if(TARGET Mak3P)
    add_test(NAME application_version COMMAND Mak3P --version)
    set_tests_properties(application_version PROPERTIES PASS_REGULAR_EXPRESSION "Mak3P ${PROJECT_VERSION}")
endif()

add_subdirectory(core)

if(TARGET audio_converter_audio)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    find_program(AUDIO_TEST_FFMPEG ffmpeg REQUIRED)
    find_program(AUDIO_TEST_FFPROBE ffprobe REQUIRED)
    set(MAK3P_AUDIO_TEST_FIXTURES "${CMAKE_CURRENT_BINARY_DIR}/audio/fixtures")

    add_subdirectory(process)
    add_subdirectory(audio)
    add_subdirectory(settings)
    add_subdirectory(batch)
    add_subdirectory(conversion)
    add_subdirectory(interface)
endif()
