cmake_minimum_required(VERSION 3.20)

project(nautilus-toolkit
    VERSION 1.0.0
    LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
include(CheckLibraryExists)
include(CheckIPOSupported)

# ── Build type defaults ──

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_BUILD_TYPE MATCHES "^(Release|MinSizeRel)$")
    add_compile_options(-ffunction-sections -fdata-sections)
    add_link_options(-Wl,--gc-sections)
endif()

set(NTK_ENABLE_IPO "AUTO" CACHE STRING "Enable CMake IPO/LTO: AUTO, ON, or OFF")
set_property(CACHE NTK_ENABLE_IPO PROPERTY STRINGS AUTO ON OFF)
set(NTK_USE_IPO OFF)
if(CMAKE_BUILD_TYPE MATCHES "^(Release|MinSizeRel)$" AND NOT NTK_ENABLE_IPO STREQUAL "OFF")
    check_ipo_supported(RESULT NTK_IPO_SUPPORTED OUTPUT NTK_IPO_LOG)
    if(NTK_ENABLE_IPO STREQUAL "ON" AND NOT NTK_IPO_SUPPORTED)
        message(FATAL_ERROR "IPO/LTO requested but not supported: ${NTK_IPO_LOG}")
    endif()
    if(NTK_IPO_SUPPORTED)
        set(NTK_USE_IPO ON)
    endif()
endif()

# ── Common warnings ──

add_library(ntk_warnings INTERFACE)
target_compile_options(ntk_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wformat -Wformat-security
)

include(CTest)

# ── Find dependencies ──

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB     REQUIRED glib-2.0>=2.74)
pkg_check_modules(GIO      REQUIRED gio-2.0>=2.74)
pkg_check_modules(GOBJECT  REQUIRED gobject-2.0)
pkg_check_modules(NAUTILUS REQUIRED libnautilus-extension-4>=43.0)
pkg_check_modules(GTK4     REQUIRED gtk4>=4.10)
pkg_check_modules(LIBADW   REQUIRED libadwaita-1>=1.4)
pkg_check_modules(JSON     REQUIRED json-glib-1.0>=1.6)
pkg_check_modules(LIBARCHIVE REQUIRED libarchive>=3.6)

# ── Nautilus extension install directory ──

pkg_get_variable(NAUTILUS_EXTENSION_DIR libnautilus-extension-4 extensiondir)
if(NOT NAUTILUS_EXTENSION_DIR)
    set(NAUTILUS_EXTENSION_DIR "${CMAKE_INSTALL_LIBDIR}/nautilus/extensions-4")
endif()
message(STATUS "Nautilus extension dir: ${NAUTILUS_EXTENSION_DIR}")

# ══════════════════════════════════════════════════════════════════════════
#  Single target: nautilus-toolkit.so
#
#  Contains everything:
#    - Nautilus extension registration (nautilus_ext.c)
#    - Unified toolkit window (toolkit_window.c)
#    - Compress module (dialog, backend, progress)
#    - Extract module (core logic, GTK UI)
# ══════════════════════════════════════════════════════════════════════════

add_library(nautilus-toolkit MODULE
    # Extension entry + unified UI
    src/nautilus_ext.c
    src/toolkit_window.c

    # Compress module
    src/compress/compress-dialog.c
    src/compress/compress-backend.c
    src/compress/compress-progress.c

    # Extract GTK UI
    src/ui_gtk/ui_gtk_window.c
    src/ui_gtk/ui_gtk_extract.c
    src/ui_gtk/ui_gtk_progress.c

    # Extract core logic
    src/core/logic.c
    src/core/exec7z.c
    src/core/pwdlib.c
    src/core/encoding.c
    src/core/path.c
    src/core/log.c
    src/core/strbuf.c
    src/core/fsutil.c
    src/core/polyglot.c
    src/core/polyglot_zip_extract.c
)

target_include_directories(nautilus-toolkit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/compress
)

target_include_directories(nautilus-toolkit SYSTEM PRIVATE
    ${GLIB_INCLUDE_DIRS}
    ${GIO_INCLUDE_DIRS}
    ${GOBJECT_INCLUDE_DIRS}
    ${NAUTILUS_INCLUDE_DIRS}
    ${GTK4_INCLUDE_DIRS}
    ${LIBADW_INCLUDE_DIRS}
    ${JSON_INCLUDE_DIRS}
    ${LIBARCHIVE_INCLUDE_DIRS}
)

target_compile_options(nautilus-toolkit PRIVATE
    ${GLIB_CFLAGS_OTHER}
    ${GIO_CFLAGS_OTHER}
    ${GOBJECT_CFLAGS_OTHER}
    ${NAUTILUS_CFLAGS_OTHER}
    ${GTK4_CFLAGS_OTHER}
    ${LIBADW_CFLAGS_OTHER}
    ${JSON_CFLAGS_OTHER}
    ${LIBARCHIVE_CFLAGS_OTHER}
    -Wno-unused-parameter
)

target_link_libraries(nautilus-toolkit PRIVATE
    ntk_warnings
    ${GLIB_LIBRARIES}
    ${GIO_LIBRARIES}
    ${GOBJECT_LIBRARIES}
    ${NAUTILUS_LIBRARIES}
    ${GTK4_LIBRARIES}
    ${LIBADW_LIBRARIES}
    ${JSON_LIBRARIES}
    ${LIBARCHIVE_LIBRARIES}
)

# iconv may be required on some platforms
check_library_exists(iconv iconv_open "iconv.h" HAVE_ICONV)
if(HAVE_ICONV)
    target_link_libraries(nautilus-toolkit PRIVATE iconv)
endif()

if(NTK_USE_IPO)
    set_property(TARGET nautilus-toolkit PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Nautilus expects a plain .so without lib prefix
set_target_properties(nautilus-toolkit PROPERTIES
    PREFIX ""
    OUTPUT_NAME "nautilus-toolkit"
)

install(TARGETS nautilus-toolkit
    LIBRARY DESTINATION "${NAUTILUS_EXTENSION_DIR}"
)

if(BUILD_TESTING)
    add_executable(test_polyglot
        tests/test_polyglot.c
        src/core/polyglot.c
        src/core/path.c
        src/core/strbuf.c
    )
    target_include_directories(test_polyglot PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    )
    target_link_libraries(test_polyglot PRIVATE ntk_warnings)
    add_test(NAME polyglot COMMAND test_polyglot)

    add_executable(test_legacy_zip
        tests/test_legacy_zip.c
        src/core/exec7z.c
        src/core/encoding.c
        src/core/strbuf.c
    )
    target_include_directories(test_legacy_zip PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    )
    target_include_directories(test_legacy_zip SYSTEM PRIVATE
        ${GLIB_INCLUDE_DIRS}
    )
    target_compile_options(test_legacy_zip PRIVATE
        ${GLIB_CFLAGS_OTHER}
    )
    target_link_libraries(test_legacy_zip PRIVATE
        ntk_warnings
        ${GLIB_LIBRARIES}
    )
    add_test(NAME legacy_zip COMMAND test_legacy_zip)
endif()
