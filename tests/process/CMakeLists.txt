add_executable(process_helper process_helper.cpp)
audio_converter_warnings(process_helper)
if(MINGW)
    target_link_options(process_helper PRIVATE -municode)
endif()

add_executable(process_tests process_tests.cpp)
target_link_libraries(process_tests PRIVATE audio_converter_audio)
audio_converter_warnings(process_tests)
foreach(test_case arguments unicode_executable pipe_limits exit_status missing_executable timeout
        cancellation callback_cleanup invalid_arguments descendant_cleanup localization)
    add_test(NAME process.${test_case} COMMAND process_tests ${test_case} $<TARGET_FILE:process_helper>)
    set_tests_properties(process.${test_case} PROPERTIES TIMEOUT 15)
endforeach()
