add_executable(batch_tests batch_tests.cpp)
target_link_libraries(batch_tests PRIVATE audio_converter_batch)
audio_converter_warnings(batch_tests)
foreach(test_case worker_policy removal inspection concurrency errors progress cancellation
        child_cancellation committed_cancellation destruction reconfigure folder_mp3 logging log_collision log_failure empty restart)
    add_test(NAME batch.${test_case} COMMAND batch_tests ${test_case} $<TARGET_FILE:process_helper>)
    set_tests_properties(batch.${test_case} PROPERTIES TIMEOUT 20)
endforeach()
if(UNIX)
    add_test(NAME batch.log_write_failure COMMAND batch_tests log_write_failure $<TARGET_FILE:process_helper>)
endif()

add_executable(batch_audio_tests batch_audio_tests.cpp)
target_link_libraries(batch_audio_tests PRIVATE audio_converter_batch)
audio_converter_warnings(batch_audio_tests)
foreach(test_case bitrate_matching bitrate_boundaries force_upsampling bitrate_sources
        effective_bitrate_validation matching_mp3 real_batch cli_batch)
    add_test(NAME batch_audio.${test_case} COMMAND batch_audio_tests ${test_case}
        "${MAK3P_AUDIO_TEST_FIXTURES}" "${AUDIO_TEST_FFMPEG}" "${AUDIO_TEST_FFPROBE}"
        $<TARGET_FILE:audio_converter_cli> $<TARGET_FILE:process_helper>)
    set_tests_properties(batch_audio.${test_case} PROPERTIES FIXTURES_REQUIRED audio_media TIMEOUT 60)
endforeach()

add_executable(inspection_cache_tests inspection_cache_tests.cpp)
target_link_libraries(inspection_cache_tests PRIVATE audio_converter_batch)
audio_converter_warnings(inspection_cache_tests)
foreach(test_case policy lifetime failures_and_cancellation promotion changed_source real_mp3 controller)
    add_test(NAME inspection_cache.${test_case} COMMAND inspection_cache_tests ${test_case}
        "${MAK3P_AUDIO_TEST_FIXTURES}" "${AUDIO_TEST_FFMPEG}" "${AUDIO_TEST_FFPROBE}")
    set_tests_properties(inspection_cache.${test_case} PROPERTIES TIMEOUT 45)
endforeach()
set_tests_properties(inspection_cache.changed_source inspection_cache.real_mp3 PROPERTIES FIXTURES_REQUIRED audio_media)
