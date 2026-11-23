add_executable(gui_controller_tests controller_tests.cpp)
target_link_libraries(gui_controller_tests PRIVATE audio_converter_batch)
audio_converter_warnings(gui_controller_tests)
foreach(test_case settings imports engine_discovery engine_recovery bundled_engine_recovery formatting
        incremental history cancellation closing real_audio real_cancel media_visibility folder_mp3_toggle)
    add_test(NAME interface.${test_case} COMMAND gui_controller_tests ${test_case}
        "${MAK3P_AUDIO_TEST_FIXTURES}" "${AUDIO_TEST_FFMPEG}" "${AUDIO_TEST_FFPROBE}" "${AUDIO_TEST_FIXTURE_FFMPEG}")
    set_tests_properties(interface.${test_case} PROPERTIES TIMEOUT 45 LABELS interface)
endforeach()
set_tests_properties(interface.real_audio interface.media_visibility interface.folder_mp3_toggle PROPERTIES FIXTURES_REQUIRED audio_media)
if(TARGET audio_converter_gui)
    add_test(NAME interface.document_resources COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/document_tests.py" "${MAK3P_DOCUMENT_SOURCE}"
        "${MAK3P_DOCUMENT_OUTPUT}" "${PROJECT_SOURCE_DIR}/tools/generate_documents.py"
        "${MAK3P_PANDOC_EXECUTABLE}")
    set_tests_properties(interface.document_resources PROPERTIES TIMEOUT 20 LABELS interface)
    add_executable(gui_window_tests window_tests.cpp)
    target_link_libraries(gui_window_tests PRIVATE audio_converter_gui)
    target_compile_definitions(gui_window_tests PRIVATE MAK3P_DOCUMENT_DIRECTORY="${MAK3P_DOCUMENT_OUTPUT}")
    add_dependencies(gui_window_tests mak3p_documents)
    audio_converter_warnings(gui_window_tests)
    add_test(NAME interface.window COMMAND gui_window_tests "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(interface.window PROPERTIES TIMEOUT 50 LABELS gui RESOURCE_LOCK desktop)
endif()
if(TARGET audio_converter_gui AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(gui_native_tests x11_tests.cpp)
    target_link_libraries(gui_native_tests PRIVATE audio_converter_gui fltk::images)
    target_compile_definitions(gui_native_tests PRIVATE MAK3P_DOCUMENT_DIRECTORY="${MAK3P_DOCUMENT_OUTPUT}"
        MAK3P_ICON_SOURCE="${PROJECT_SOURCE_DIR}/resources/icons/mak3p.ico")
    add_dependencies(gui_native_tests mak3p_documents)
    audio_converter_warnings(gui_native_tests)
    foreach(test_case constraints documents pickers drop icons folder)
        add_test(NAME interface.native_${test_case} COMMAND gui_native_tests ${test_case}
            "${MAK3P_AUDIO_TEST_FIXTURES}" "${AUDIO_TEST_FFMPEG}" "${AUDIO_TEST_FFPROBE}")
        set_tests_properties(interface.native_${test_case} PROPERTIES TIMEOUT 45 LABELS gui RESOURCE_LOCK desktop)
    endforeach()
    set_tests_properties(interface.native_drop interface.native_folder PROPERTIES FIXTURES_REQUIRED audio_media)
    set_tests_properties(interface.native_pickers PROPERTIES ENVIRONMENT "GDK_BACKEND=x11")
endif()
