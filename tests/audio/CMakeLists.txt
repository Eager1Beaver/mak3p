add_test(NAME audio.fixtures COMMAND Python3::Interpreter
    "${CMAKE_CURRENT_SOURCE_DIR}/generate_fixtures.py" "${AUDIO_TEST_FFMPEG}" "${MAK3P_AUDIO_TEST_FIXTURES}")
set_tests_properties(audio.fixtures PROPERTIES FIXTURES_SETUP audio_media TIMEOUT 90)

add_executable(audio_tests audio_tests.cpp)
target_link_libraries(audio_tests PRIVATE audio_converter_audio TagLib::tag)
target_include_directories(audio_tests SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
audio_converter_warnings(audio_tests)
set(audio_test_cases policy probe_invalid probe_disguised probe_image_fallback probe_streams probe_stream_fallback engine_errors
        mp3_decisions mp3_uncertain mp3_bounded_analysis bitrates channel_modes resampling formats conversion_protection
        stale_inspection settings_changed failed_conversion validation_failure conversion_cancellation
        progress unknown_progress replaced_temporary unicode_conversion metadata_text metadata_artwork metadata_totals metadata_omissions
        metadata_absent metadata_containers readonly_source cli_smoke)
if(WIN32)
    list(APPEND audio_test_cases long_windows_paths)
endif()
foreach(test_case IN LISTS audio_test_cases)
    add_test(NAME audio.${test_case} COMMAND audio_tests ${test_case} "${MAK3P_AUDIO_TEST_FIXTURES}"
        $<TARGET_FILE:process_helper> "${AUDIO_TEST_FFMPEG}" "${AUDIO_TEST_FFPROBE}"
        $<TARGET_FILE:audio_converter_cli>)
    set_tests_properties(audio.${test_case} PROPERTIES FIXTURES_REQUIRED audio_media TIMEOUT 60)
endforeach()
