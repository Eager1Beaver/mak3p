# MSVC 19.52 provides copysign as an intrinsic.
set(header "${FLTK_SOURCE_DIR}/FL/math.h")
file(READ "${header}" content)
set(fallback "inline double copysign(double a, double b) {return b<0 ? -a : a;}")
set(guarded "#if !defined(_MSC_VER) || _MSC_VER < 1952\n${fallback}\n#endif")

string(FIND "${content}" "${guarded}" patched)
if(patched EQUAL -1)
    string(REPLACE "${fallback}" "${guarded}" updated "${content}")
    if(updated STREQUAL content)
        message(FATAL_ERROR "Cannot find the FLTK copysign fallback in ${header}")
    endif()
    file(WRITE "${header}" "${updated}")
endif()
