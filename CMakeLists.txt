cmake_minimum_required(VERSION 3.20)
project(arcradient VERSION 0.1.0 LANGUAGES C)

# Strict Pure C11 standard without GNU VLA bloat
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(ARCR_LEGACY_X86_WIN2000 "Target Windows 2000 SP4 32-bit x86 + SSE1 baseline" OFF)
option(ARCR_BASELINE_X64_SSE3  "Target x86-64 SSE3 baseline (no SSE4.x/AVX required)" ON)
option(ARCR_ENABLE_FREETYPE    "Enable optional FreeType2 CJK glyph backend on POSIX" ON)
option(ARCR_ENABLE_LIBPNG      "Enable optional libpng PNG snapshot export on POSIX" ON)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wvla)
    if(ARCR_LEGACY_X86_WIN2000)
        add_compile_options(-m32 -march=pentium3 -msse -mno-sse2)
    elseif(ARCR_BASELINE_X64_SSE3 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        # Baseline x64 guarantees SSE2+SSE3 without requiring SSE4.x or AVX!
        add_compile_options(-msse3 -mno-ssse3 -mno-sse4.1 -mno-sse4.2 -mno-avx)
    endif()
endif()

if(WIN32)
    add_compile_definitions(
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
    if(MINGW)
        # Eliminate external libgcc_s_*.dll runtime dependency on vanilla Windows 2000 SP4+
        add_link_options(-static-libgcc)
    endif()
    if(ARCR_LEGACY_X86_WIN2000)
        add_compile_definitions(WINVER=0x0500 _WIN32_WINNT=0x0500)
        if(MINGW OR CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            add_link_options(
                "LINKER:--major-os-version,5"
                "LINKER:--minor-os-version,0"
                "LINKER:--major-subsystem-version,5"
                "LINKER:--minor-subsystem-version,0"
            )
        endif()
    endif()
endif()

# Platform & header interface shared across Engine Core and On-Demand Dynamic Modules
add_library(arcr_platform_deps INTERFACE)
target_include_directories(arcr_platform_deps INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/base"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform"
)

# Base + Platform object library (compiled once with PIC; zero duplicate import-lib collisions)
set(ARCR_BASE_SOURCES
    base/arcr_arena.c
    base/arcr_atom.c
    base/arcr_str.c
    base/arcr_utf8.c
    base/arcr_cpuid.c
)

if(WIN32)
    set(ARCR_PLATFORM_SOURCES platform/win32/arcr_platform_win32.c)
else()
    set(ARCR_PLATFORM_SOURCES platform/posix/arcr_platform_posix.c)
endif()

add_library(arcr_base OBJECT ${ARCR_BASE_SOURCES} ${ARCR_PLATFORM_SOURCES})
set_target_properties(arcr_base PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(arcr_base PUBLIC arcr_platform_deps)

if(WIN32)
    target_link_libraries(arcr_platform_deps INTERFACE gdi32 user32 advapi32 imm32 ws2_32)
else()
    target_link_libraries(arcr_platform_deps INTERFACE ${CMAKE_DL_LIBS})
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND AND ARCR_ENABLE_FREETYPE)
        pkg_check_modules(FREETYPE QUIET freetype2)
        if(FREETYPE_FOUND)
            target_compile_definitions(arcr_base PRIVATE ARCR_HAVE_FREETYPE=1)
            target_include_directories(arcr_base PRIVATE ${FREETYPE_INCLUDE_DIRS})
            target_link_libraries(arcr_platform_deps INTERFACE ${FREETYPE_LIBRARIES})
        endif()
    endif()
    if(PkgConfig_FOUND AND ARCR_ENABLE_LIBPNG)
        pkg_check_modules(LIBPNG QUIET libpng)
        if(LIBPNG_FOUND)
            target_compile_definitions(arcr_base PRIVATE ARCR_HAVE_LIBPNG=1)
            target_include_directories(arcr_base PRIVATE ${LIBPNG_INCLUDE_DIRS})
            target_link_libraries(arcr_platform_deps INTERFACE ${LIBPNG_LIBRARIES})
        endif()
    endif()
endif()

# Unified Lightweight Engine Core (base + core + css + layout + paint + accel router)
add_library(arcr_engine SHARED
    $<TARGET_OBJECTS:arcr_base>
    accel/arcr_accel.c
    core/arcr_dom.c
    core/arcr_html_parser.c
    css/arcr_css.c
    layout/arcr_layout.c
    paint/arcr_paint.c
    core/arcr_engine.c
)
target_link_libraries(arcr_engine PUBLIC arcr_platform_deps)
set_target_properties(arcr_engine PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
if(MINGW)
    target_link_options(arcr_engine PRIVATE "LINKER:--export-all-symbols")
endif()

# On-Demand Dynamically Loaded Feature Modules (Loaded ONLY when page requests them!)
add_library(arcr_net SHARED net/arcr_net.c $<TARGET_OBJECTS:arcr_base>)
target_link_libraries(arcr_net PRIVATE arcr_platform_deps)

add_library(arcr_js_portable SHARED js/arcr_js_portable.c $<TARGET_OBJECTS:arcr_base>)
target_link_libraries(arcr_js_portable PRIVATE arcr_platform_deps)

add_library(arcr_media SHARED media/arcr_media.c $<TARGET_OBJECTS:arcr_base>)
target_link_libraries(arcr_media PRIVATE arcr_platform_deps)

add_library(arcr_devtools SHARED devtools/arcr_devtools.c $<TARGET_OBJECTS:arcr_base>)
target_link_libraries(arcr_devtools PRIVATE arcr_platform_deps)

# Arcradient Browser Shell (Decoupled from internal Engine structs via arcradient_embed.h)
add_library(arcr_shell STATIC shell/arcr_shell.c)
target_link_libraries(arcr_shell PUBLIC arcr_engine arcr_platform_deps)

add_executable(arcradient shell/main.c)
target_link_libraries(arcradient PRIVATE arcr_shell)

# Verification & Regression Suite
enable_testing()
add_executable(arcr_tests tests/test_suite.c)
target_link_libraries(arcr_tests PRIVATE arcr_shell arcr_engine arcr_platform_deps)
add_test(NAME ArcradientCoreSuite COMMAND arcr_tests "$<TARGET_FILE_DIR:arcr_net>")

if(WIN32)
    # Match arcr_platform_format_module_path ("%s\\%s.dll") without MinGW "lib" prefix
    set_target_properties(
        arcr_engine arcr_net arcr_js_portable arcr_media arcr_devtools
        PROPERTIES
            PREFIX ""
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    set_target_properties(
        arcradient arcr_tests
        PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
endif()
