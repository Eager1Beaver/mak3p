add_executable(settings_tests settings_tests.cpp)
target_link_libraries(settings_tests PRIVATE audio_converter_batch)
audio_converter_warnings(settings_tests)
foreach(test_case defaults round_trip invalid_entries paths limits persistence fallback fallback_migration branding
        unavailable_io readonly_config owned_files sanitization)
    add_test(NAME settings.${test_case} COMMAND settings_tests ${test_case})
    set_tests_properties(settings.${test_case} PROPERTIES TIMEOUT 15)
endforeach()
