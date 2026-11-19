# Generate self-contained documents with tools used only on the build machine.
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
find_program(MAK3P_PANDOC_EXECUTABLE NAMES pandoc REQUIRED)

set(MAK3P_DOCUMENT_SOURCE "${PROJECT_SOURCE_DIR}/resources/documents")
set(MAK3P_DOCUMENT_OUTPUT "${PROJECT_BINARY_DIR}/generated/documents")
get_filename_component(MAK3P_PACKAGE_SHARE "${TagLib_DIR}" DIRECTORY)
find_file(MAK3P_TAGLIB_LGPL NAMES copyright COPYING.LGPL
    HINTS "${TagLib_DIR}" REQUIRED NO_DEFAULT_PATH)
find_file(MAK3P_TAGLIB_MPL NAMES COPYING.MPL
    HINTS "${TagLib_DIR}" REQUIRED NO_DEFAULT_PATH)
find_file(MAK3P_UTFCPP_LICENSE NAMES copyright
    HINTS "${MAK3P_PACKAGE_SHARE}/utfcpp" REQUIRED NO_DEFAULT_PATH)
find_file(MAK3P_ZLIB_LICENSE NAMES copyright
    HINTS "${MAK3P_PACKAGE_SHARE}/zlib" REQUIRED NO_DEFAULT_PATH)

set(MAK3P_DOCUMENT_NAMES help licenses changelog)
set(MAK3P_DOCUMENT_SOURCES)
set(MAK3P_DOCUMENT_FILES)
foreach(name IN LISTS MAK3P_DOCUMENT_NAMES)
    list(APPEND MAK3P_DOCUMENT_SOURCES "${MAK3P_DOCUMENT_SOURCE}/${name}.md")
    list(APPEND MAK3P_DOCUMENT_FILES "${MAK3P_DOCUMENT_OUTPUT}/${name}.html"
        "${MAK3P_DOCUMENT_OUTPUT}/${name}.md")
endforeach()
set(MAK3P_NOTICE_NAMES fltk-license fltk-utf8-license taglib-lgpl taglib-mpl
    json-license utfcpp-license zlib-license)
foreach(name IN LISTS MAK3P_NOTICE_NAMES)
    list(APPEND MAK3P_DOCUMENT_FILES "${MAK3P_DOCUMENT_OUTPUT}/${name}.html"
        "${MAK3P_DOCUMENT_OUTPUT}/licenses/${name}.txt")
endforeach()
set(MAK3P_NOTICE_FILES "${fltk_SOURCE_DIR}/COPYING" "${fltk_SOURCE_DIR}/src/xutf8/COPYING"
    "${MAK3P_TAGLIB_LGPL}" "${MAK3P_TAGLIB_MPL}"
    "${PROJECT_SOURCE_DIR}/third_party/nlohmann/LICENSE.MIT"
    "${MAK3P_UTFCPP_LICENSE}" "${MAK3P_ZLIB_LICENSE}")

add_custom_command(OUTPUT ${MAK3P_DOCUMENT_FILES}
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/generate_documents.py"
        --pandoc "${MAK3P_PANDOC_EXECUTABLE}"
        --source "${MAK3P_DOCUMENT_SOURCE}" --output "${MAK3P_DOCUMENT_OUTPUT}"
        --notice fltk-license "FLTK license and exceptions" "${fltk_SOURCE_DIR}/COPYING"
        --notice fltk-utf8-license "FLTK UTF-8 code notices" "${fltk_SOURCE_DIR}/src/xutf8/COPYING"
        --notice taglib-lgpl "TagLib: GNU LGPL 2.1" "${MAK3P_TAGLIB_LGPL}"
        --notice taglib-mpl "TagLib: Mozilla Public License 1.1" "${MAK3P_TAGLIB_MPL}"
        --notice json-license "JSON for Modern C++: MIT License" "${PROJECT_SOURCE_DIR}/third_party/nlohmann/LICENSE.MIT"
        --notice utfcpp-license "UTF8-CPP: Boost Software License 1.0" "${MAK3P_UTFCPP_LICENSE}"
        --notice zlib-license "zlib license" "${MAK3P_ZLIB_LICENSE}"
    DEPENDS ${MAK3P_DOCUMENT_SOURCES} ${MAK3P_NOTICE_FILES}
        "${PROJECT_SOURCE_DIR}/tools/generate_documents.py"
    COMMENT "Generating local Markdown documents and component licenses"
    VERBATIM)
add_custom_target(mak3p_documents DEPENDS ${MAK3P_DOCUMENT_FILES})
add_dependencies(Mak3P mak3p_documents)
set_property(TARGET Mak3P APPEND PROPERTY LINK_DEPENDS ${MAK3P_DOCUMENT_FILES})
add_custom_command(TARGET Mak3P POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${MAK3P_DOCUMENT_OUTPUT}" "$<TARGET_FILE_DIR:Mak3P>/documents"
    COMMAND "${CMAKE_COMMAND}" -E rm -f "$<TARGET_FILE_DIR:Mak3P>/documents/help.txt"
        "$<TARGET_FILE_DIR:Mak3P>/documents/licenses.txt" "$<TARGET_FILE_DIR:Mak3P>/documents/changelog.txt"
    VERBATIM)
