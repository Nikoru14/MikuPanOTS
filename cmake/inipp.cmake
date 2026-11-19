include(FetchContent)

FetchContent_Declare(
        inipp
        GIT_REPOSITORY https://github.com/mcmtroffaes/inipp.git
        GIT_TAG 1.0.13
)

FetchContent_MakeAvailable(inipp)

if(PS5)
    # inipp builds std::locale{"C"} by name, and libc++ fills a named locale
    # from the console's localeconv_l, which hands back garbage pointers: the
    # title crashed reading mikupan.ini. std::locale::classic() is the same
    # "C" locale without that lookup. The header is copied with that one
    # change and used in place of the original.
    set(MIKUPAN_PS5_INIPP_DIR "${CMAKE_BINARY_DIR}/ps5/inipp")
    file(READ "${inipp_SOURCE_DIR}/inipp/inipp.h" MIKUPAN_INIPP_HEADER)
    string(REPLACE "const std::locale loc{\"C\"};" "const std::locale loc{std::locale::classic()};"
            MIKUPAN_INIPP_HEADER "${MIKUPAN_INIPP_HEADER}")
    string(REPLACE "const std::locale loc{ \"C\" };" "const std::locale loc{std::locale::classic()};"
            MIKUPAN_INIPP_HEADER "${MIKUPAN_INIPP_HEADER}")
    string(FIND "${MIKUPAN_INIPP_HEADER}" "\"C\"" MIKUPAN_INIPP_NAMED_LOCALE)
    if(NOT MIKUPAN_INIPP_NAMED_LOCALE EQUAL -1)
        message(FATAL_ERROR "MikuPan: inipp.h still builds a named \"C\" locale; update cmake/inipp.cmake")
    endif()
    file(WRITE "${MIKUPAN_PS5_INIPP_DIR}/inipp.h" "${MIKUPAN_INIPP_HEADER}")
    set_target_properties(inipp PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${MIKUPAN_PS5_INIPP_DIR}"
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${MIKUPAN_PS5_INIPP_DIR}")
endif()
