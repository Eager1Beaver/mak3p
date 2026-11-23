function(core_test_cases target)
    add_executable(${target} ${target}.cpp)
    target_link_libraries(${target} PRIVATE audio_converter_core)
    audio_converter_warnings(${target})
    foreach(test_case IN LISTS ARGN)
        add_test(NAME core.${test_case} COMMAND ${target} ${test_case})
        set_tests_properties(core.${test_case} PROPERTIES TIMEOUT 30)
    endforeach()
endfunction()

core_test_cases(models_tests
    queue_normalization queue_origin queue_distinct_sources queue_alias
    queue_case_rules queue_clear job_success job_skips job_failure
    job_cancellation job_reset job_invalid_changes invalid_source_paths
)
core_test_cases(output_naming_tests
    naming_collisions naming_source_directories naming_invalid_destination naming_clear
    naming_gaps naming_relative naming_unicode naming_long_name naming_alias
    naming_occupied_links naming_invalid_sources naming_case_rules naming_windows_sanitization
)
core_test_cases(output_safety_tests
    output_commit output_late_conflict output_reserved_retry output_directory_conflict output_symlink_conflict
    output_cleanup output_unwinding output_empty output_discarded output_unique_temporary
    output_replaced_temporary output_missing_temporary output_symlink_temporary
    output_concurrent output_unwritable output_unicode output_failure_cleanup
)
