set(MAK3P_TEST_SAMPLE_DIR "" CACHE PATH "Directory containing optional APE and hybrid WavPack test samples")

add_executable(core_validation_runner runner.cpp)
target_link_libraries(core_validation_runner PRIVATE audio_converter_batch TagLib::tag)
target_include_directories(core_validation_runner SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
audio_converter_warnings(core_validation_runner)

set(validation_fixtures "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
set(validation_artifacts "${CMAKE_CURRENT_BINARY_DIR}/artifacts")
add_test(NAME validation.fixtures COMMAND Python3::Interpreter
    "${CMAKE_CURRENT_SOURCE_DIR}/generate_fixtures.py" "${AUDIO_TEST_FIXTURE_FFMPEG}"
    "${AUDIO_TEST_FFPROBE}" "${validation_fixtures}"
    "${MAK3P_TEST_SAMPLE_DIR}")
set_tests_properties(validation.fixtures PROPERTIES FIXTURES_SETUP validation_media TIMEOUT 120)
set(conversion_scenarios matrix_mono matrix_stereo mixed_batch corrupted_inputs bitrate_policy
        parallel_workers hundred_files destinations signal_quality metadata malformed_metadata
        cancellation cli_workflow cli_cancellation encoding_failure folder_import)
if(MAK3P_TEST_SAMPLE_DIR)
    list(APPEND conversion_scenarios wavpack_hybrid)
endif()
foreach(scenario IN LISTS conversion_scenarios)
    add_test(NAME validation.${scenario} COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/validate.py" "${scenario}"
        $<TARGET_FILE:core_validation_runner> $<TARGET_FILE:audio_converter_cli>
        "${AUDIO_TEST_FFMPEG}" "${AUDIO_TEST_FFPROBE}"
        "${validation_fixtures}" "${validation_artifacts}" "${MAK3P_AUDIO_TEST_FIXTURES}")
    set_tests_properties(validation.${scenario} PROPERTIES
        FIXTURES_REQUIRED validation_media TIMEOUT 180 PROCESSORS 2)
endforeach()
set_tests_properties(validation.folder_import PROPERTIES FIXTURES_REQUIRED "validation_media;audio_media")
