cmake_minimum_required(VERSION 3.20)
project(waterscape VERSION 2.1.0 LANGUAGES C)

# Strict Pure C11 standard without GNU VLA bloat
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(ARCR_LEGACY_X86_WIN2000 "Target Windows 2000 SP4 32-bit x86 + SSE1 baseline" OFF)
option(ARCR_BASELINE_X64_SSE3  "Target x86-64 SSE3 baseline (no SSE4.x/AVX required; AVX2 runtime-dispatched)" ON)
option(ARCR_ENABLE_FREETYPE    "Enable optional FreeType2 CJK/Mono glyph backend on POSIX" ON)
option(ARCR_ENABLE_LIBPNG      "Enable optional libpng PNG export on POSIX (falls back to Pure C PNG codec)" ON)
option(ARCR_ENABLE_OPENSSL     "Enable OpenSSL TLS 1.2/1.3 HTTPS client on POSIX" ON)
option(WATERSCAPE_ENABLE_X11   "Enable native X11 + MIT-SHM desktop window presenter on POSIX" ON)
option(ARCR_ENABLE_QUICKJS_COMPARE "Build optional QuickJS-NG comparison module (OFF in shipping)" OFF)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        $<$<COMPILE_LANGUAGE:C>:-Wall>
        $<$<COMPILE_LANGUAGE:C>:-Wextra>
        $<$<COMPILE_LANGUAGE:C>:-Wpedantic>
        $<$<COMPILE_LANGUAGE:C>:-Wvla>
    )
    if(ARCR_LEGACY_X86_WIN2000)
        add_compile_options(
            $<$<COMPILE_LANGUAGE:C>:-m32>
            $<$<COMPILE_LANGUAGE:C>:-march=pentium3>
            $<$<COMPILE_LANGUAGE:C>:-msse>
            $<$<COMPILE_LANGUAGE:C>:-mno-sse2>
        )
    elseif(ARCR_BASELINE_X64_SSE3 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        # Baseline x64 guarantees SSE2+SSE3 without requiring SSE4.x or AVX on older x64 CPUs!
        add_compile_options(
            $<$<COMPILE_LANGUAGE:C>:-msse3>
            $<$<COMPILE_LANGUAGE:C>:-mno-ssse3>
            $<$<COMPILE_LANGUAGE:C>:-mno-sse4.1>
            $<$<COMPILE_LANGUAGE:C>:-mno-sse4.2>
            $<$<COMPILE_LANGUAGE:C>:-mno-avx>
        )
    endif()
endif()

if(WIN32)
    enable_language(RC)
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
else()
    add_compile_definitions(_POSIX_C_SOURCE=200809L)
endif()

# =============================================================================
# Vendored Permissive Pure C11 Foundations (Brotli, libwebp, QuickJS-NG, stb, NanoSVG)
# Compiled with -w so our own Waterscape/Arcradient sources stay -Wall -Wextra -Wpedantic -Wvla!
# =============================================================================
file(GLOB ARCR_BROTLI_SRCS
    "third_party/brotli/common/*.c"
    "third_party/brotli/dec/*.c"
)
add_library(arcr_brotli_obj OBJECT ${ARCR_BROTLI_SRCS})
set_target_properties(arcr_brotli_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(arcr_brotli_obj PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/brotli/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/brotli"
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(arcr_brotli_obj PRIVATE -w)
endif()

set(ARCR_WEBP_SRCS
    third_party/libwebp/src/dec/alpha_dec.c
    third_party/libwebp/src/dec/buffer_dec.c
    third_party/libwebp/src/dec/frame_dec.c
    third_party/libwebp/src/dec/idec_dec.c
    third_party/libwebp/src/dec/io_dec.c
    third_party/libwebp/src/dec/quant_dec.c
    third_party/libwebp/src/dec/tree_dec.c
    third_party/libwebp/src/dec/vp8_dec.c
    third_party/libwebp/src/dec/vp8l_dec.c
    third_party/libwebp/src/dec/webp_dec.c
    third_party/libwebp/src/dsp/alpha_processing.c
    third_party/libwebp/src/dsp/cpu.c
    third_party/libwebp/src/dsp/dec.c
    third_party/libwebp/src/dsp/dec_clip_tables.c
    third_party/libwebp/src/dsp/filters.c
    third_party/libwebp/src/dsp/lossless.c
    third_party/libwebp/src/dsp/rescaler.c
    third_party/libwebp/src/dsp/upsampling.c
    third_party/libwebp/src/dsp/yuv.c
    third_party/libwebp/src/utils/bit_reader_utils.c
    third_party/libwebp/src/utils/color_cache_utils.c
    third_party/libwebp/src/utils/filters_utils.c
    third_party/libwebp/src/utils/huffman_utils.c
    third_party/libwebp/src/utils/quant_levels_dec_utils.c
    third_party/libwebp/src/utils/random_utils.c
    third_party/libwebp/src/utils/rescaler_utils.c
    third_party/libwebp/src/utils/thread_utils.c
    third_party/libwebp/src/utils/utils.c
)
if(NOT ARCR_LEGACY_X86_WIN2000)
    list(APPEND ARCR_WEBP_SRCS
        third_party/libwebp/src/dsp/alpha_processing_sse2.c
        third_party/libwebp/src/dsp/dec_sse2.c
        third_party/libwebp/src/dsp/filters_sse2.c
        third_party/libwebp/src/dsp/lossless_sse2.c
        third_party/libwebp/src/dsp/rescaler_sse2.c
        third_party/libwebp/src/dsp/upsampling_sse2.c
        third_party/libwebp/src/dsp/yuv_sse2.c
    )
endif()
add_library(arcr_webp_obj OBJECT ${ARCR_WEBP_SRCS})
set_target_properties(arcr_webp_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(arcr_webp_obj PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libwebp"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libwebp/src"
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(arcr_webp_obj PRIVATE -w)
endif()

if(ARCR_ENABLE_QUICKJS_COMPARE)
    add_library(arcr_quickjs_obj OBJECT
        third_party/quickjs/quickjs.c
        third_party/quickjs/libregexp.c
        third_party/quickjs/libunicode.c
        third_party/quickjs/dtoa.c
    )
    set_target_properties(arcr_quickjs_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(arcr_quickjs_obj PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs")
    target_compile_definitions(arcr_quickjs_obj PRIVATE QUICKJS_NG_BUILD=1)
    if(NOT WIN32)
        target_compile_definitions(arcr_quickjs_obj PRIVATE _GNU_SOURCE=1 _DEFAULT_SOURCE=1)
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(arcr_quickjs_obj PRIVATE -w)
    endif()
endif()

# =============================================================================
# Tier 0 & Tier 1: Zero-Dependency Base + Strict OS Platform Abstraction
# =============================================================================
add_library(arcr_platform_deps INTERFACE)
target_include_directories(arcr_platform_deps INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/base"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/brotli/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libwebp/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nanosvg"
)

set(ARCR_BASE_SOURCES
    base/arcr_arena.c
    base/arcr_atom.c
    base/arcr_str.c
    base/arcr_utf8.c
    base/arcr_cpuid.c
    base/arcr_codec.c
    base/arcr_threadpool.c
)

if(WIN32)
    set(ARCR_PLATFORM_SOURCES platform/win32/arcr_platform_win32.c)
else()
    set(ARCR_PLATFORM_SOURCES platform/posix/arcr_platform_posix.c)
endif()

add_library(arcr_base OBJECT ${ARCR_BASE_SOURCES} ${ARCR_PLATFORM_SOURCES})
set_target_properties(arcr_base PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(arcr_base PRIVATE ARCR_HAVE_MODERN_CODECS=1)
target_link_libraries(arcr_base PUBLIC arcr_platform_deps)

set(ARCR_CORE_OBJECTS
    $<TARGET_OBJECTS:arcr_base>
    $<TARGET_OBJECTS:arcr_brotli_obj>
    $<TARGET_OBJECTS:arcr_webp_obj>
)

if(WIN32)
    # Native Win32 system DLLs present since Windows 2000 SP4 (zero winpthread/vcruntime!)
    target_link_libraries(arcr_platform_deps INTERFACE
        gdi32 user32 advapi32 shell32 ole32 comctl32 imm32 ws2_32 winmm secur32 crypt32
    )
else()
    find_package(Threads REQUIRED)
    target_link_libraries(arcr_platform_deps INTERFACE ${CMAKE_DL_LIBS} Threads::Threads m)

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

# =============================================================================
# Tier 2: Runtime-Dispatched AVX2 SIMD Kernel Object (Guarded by CPUID at Runtime)
# =============================================================================
add_library(arcr_accel_avx2_obj OBJECT accel/arcr_accel_avx2.c)
set_target_properties(arcr_accel_avx2_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(arcr_accel_avx2_obj PUBLIC arcr_platform_deps)
if((CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
   AND (NOT ARCR_LEGACY_X86_WIN2000)
   AND (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"))
    target_compile_options(arcr_accel_avx2_obj PRIVATE -mavx2)
endif()

# =============================================================================
# Tier 3: Unified Clean-Room Web Engine Core (DOM + CSS3 + Layout + 64x64 Tile Paint)
# =============================================================================
add_library(arcr_engine SHARED
    ${ARCR_CORE_OBJECTS}
    $<TARGET_OBJECTS:arcr_accel_avx2_obj>
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

if(UNIX AND NOT APPLE)
    set(CMAKE_BUILD_RPATH "$ORIGIN")
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif()

# =============================================================================
# Tier 4: On-Demand Dynamically Loaded Engine Modules (.dll / .so)
# =============================================================================
add_library(arcr_net SHARED net/arcr_net.c)
target_link_libraries(arcr_net PRIVATE arcr_engine arcr_platform_deps)
if((NOT WIN32) AND PkgConfig_FOUND AND ARCR_ENABLE_OPENSSL)
    pkg_check_modules(OPENSSL QUIET openssl)
    if(OPENSSL_FOUND)
        target_compile_definitions(arcr_net PRIVATE ARCR_HAVE_OPENSSL=1)
        target_include_directories(arcr_net PRIVATE ${OPENSSL_INCLUDE_DIRS})
        target_link_libraries(arcr_net PRIVATE ${OPENSSL_LIBRARIES})
    endif()
endif()

set(ARCR_AJS_SOURCES
    js/vm/ajs_utf16.c
    js/vm/ajs_gc.c
    js/vm/ajs_object.c
    js/vm/ajs_lexer.c
    js/vm/ajs_parser.c
    js/vm/ajs_compiler.c
    js/vm/ajs_interp.c
    js/vm/ajs_api.c
    js/builtins/ajs_builtins.c
    js/builtins/ajs_string_array.c
    js/builtins/ajs_collections.c
    js/builtins/ajs_regexp.c
    js/builtins/ajs_json.c
    js/builtins/ajs_promise.c
    js/builtins/ajs_typedarray.c
    js/builtins/ajs_intl.c
    js/modules/ajs_module.c
)

set(ARCR_WEB_BINDINGS_SOURCES
    web/bindings/web_dom_bridge.c
    web/bindings/web_canvas2d.c
    web/bindings/web_crypto_streams.c
    web/bindings/web_bootstrap.c
    web/bindings/web_js_module.c
)

add_library(arcr_js_portable SHARED
    ${ARCR_AJS_SOURCES}
    ${ARCR_WEB_BINDINGS_SOURCES}
)
target_include_directories(arcr_js_portable PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/js/vm"
)
target_link_libraries(arcr_js_portable PRIVATE arcr_engine arcr_platform_deps)

# Standalone Clean-Room Pure C11 ArcradientJS CLI & Test262 Host (Zero DOM/Browser Coupling)
add_executable(arcr_js
    tools/arcr_js_cli.c
    ${ARCR_AJS_SOURCES}
)
target_include_directories(arcr_js PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/js/vm"
)
if(NOT WIN32)
    target_link_libraries(arcr_js PRIVATE m)
endif()

if(ARCR_ENABLE_QUICKJS_COMPARE)
    add_library(arcr_js_quickjs_compare SHARED
        js/arcr_js_portable.c
        $<TARGET_OBJECTS:arcr_quickjs_obj>
    )
    target_include_directories(arcr_js_quickjs_compare PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs")
    target_link_libraries(arcr_js_quickjs_compare PRIVATE arcr_engine arcr_platform_deps)
endif()

add_library(arcr_media SHARED media/arcr_media.c)
target_link_libraries(arcr_media PRIVATE arcr_engine arcr_platform_deps)

add_library(arcr_devtools SHARED devtools/arcr_devtools.c)
target_link_libraries(arcr_devtools PRIVATE arcr_engine arcr_platform_deps)

# =============================================================================
# Tier 5: Official Native C11 Telemetry Plugin & WebExtensions (.wsx) Host
# =============================================================================
add_library(ws_plugin_perf_hud SHARED plugins/perf_hud/ws_plugin_perf_hud.c)
target_link_libraries(ws_plugin_perf_hud PRIVATE arcr_engine arcr_platform_deps)

# =============================================================================
# Tier 7 (Part A): Host Build Tools -> Procedural Brand Assets & .wsx Add-ons
# Note: Compiled WITHOUT ARCR_HAVE_MODERN_CODECS so host tools build instantly!
# =============================================================================
set(WS_BRAND_DIR  "${CMAKE_BINARY_DIR}/brand")
set(WS_ADDONS_DIR "${CMAKE_BINARY_DIR}/addons")

if(CMAKE_CROSSCOMPILING)
    find_program(WS_HOST_CC NAMES cc gcc clang REQUIRED)
    set(WS_HOST_BIN_DIR "${CMAKE_BINARY_DIR}/host_tools")
    file(MAKE_DIRECTORY "${WS_HOST_BIN_DIR}")

    set(GEN_BRAND_TOOL      "${WS_HOST_BIN_DIR}/gen_brand_assets")
    set(PACK_WSX_TOOL       "${WS_HOST_BIN_DIR}/pack_wsx")
    set(PACK_INSTALLER_TOOL "${WS_HOST_BIN_DIR}/pack_installer")

    add_custom_command(
        OUTPUT "${GEN_BRAND_TOOL}"
        COMMAND "${WS_HOST_CC}" -std=c11 -O2 -D_POSIX_C_SOURCE=200809L
                -I"${CMAKE_CURRENT_SOURCE_DIR}/include"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_brand_assets.c"
                "${CMAKE_CURRENT_SOURCE_DIR}/base/arcr_codec.c"
                "${CMAKE_CURRENT_SOURCE_DIR}/base/arcr_str.c"
                -lm -o "${GEN_BRAND_TOOL}"
        DEPENDS tools/gen_brand_assets.c base/arcr_codec.c base/arcr_str.c
        COMMENT "Building host tool gen_brand_assets"
    )
    add_custom_command(
        OUTPUT "${PACK_WSX_TOOL}"
        COMMAND "${WS_HOST_CC}" -std=c11 -O2 -D_POSIX_C_SOURCE=200809L
                -I"${CMAKE_CURRENT_SOURCE_DIR}/include"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_wsx.c"
                "${CMAKE_CURRENT_SOURCE_DIR}/base/arcr_codec.c"
                "${CMAKE_CURRENT_SOURCE_DIR}/base/arcr_str.c"
                -o "${PACK_WSX_TOOL}"
        DEPENDS tools/pack_wsx.c base/arcr_codec.c base/arcr_str.c
        COMMENT "Building host tool pack_wsx"
    )
    add_custom_command(
        OUTPUT "${PACK_INSTALLER_TOOL}"
        COMMAND "${WS_HOST_CC}" -std=c11 -O2 -D_POSIX_C_SOURCE=200809L
                -I"${CMAKE_CURRENT_SOURCE_DIR}/include"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_installer.c"
                "${CMAKE_CURRENT_SOURCE_DIR}/base/arcr_codec.c"
                "${CMAKE_CURRENT_SOURCE_DIR}/base/arcr_str.c"
                -o "${PACK_INSTALLER_TOOL}"
        DEPENDS tools/pack_installer.c base/arcr_codec.c base/arcr_str.c
        COMMENT "Building host tool pack_installer"
    )
else()
    add_executable(gen_brand_assets tools/gen_brand_assets.c base/arcr_codec.c base/arcr_str.c)
    target_include_directories(gen_brand_assets PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    if(NOT WIN32)
        target_link_libraries(gen_brand_assets PRIVATE m)
    endif()

    add_executable(pack_wsx tools/pack_wsx.c base/arcr_codec.c base/arcr_str.c)
    target_include_directories(pack_wsx PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

    add_executable(pack_installer tools/pack_installer.c base/arcr_codec.c base/arcr_str.c)
    target_include_directories(pack_installer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

    set(GEN_BRAND_TOOL      "$<TARGET_FILE:gen_brand_assets>")
    set(PACK_WSX_TOOL       "$<TARGET_FILE:pack_wsx>")
    set(PACK_INSTALLER_TOOL "$<TARGET_FILE:pack_installer>")
endif()

add_custom_command(
    OUTPUT
        "${WS_BRAND_DIR}/waterscape.ico"
        "${WS_BRAND_DIR}/waterscape_256.png"
        "${WS_BRAND_DIR}/waterscape_48.png"
        "${WS_BRAND_DIR}/waterscape.svg"
        "${WS_BRAND_DIR}/wizard_banner.bmp"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${WS_BRAND_DIR}"
    COMMAND "${GEN_BRAND_TOOL}" "${WS_BRAND_DIR}"
    DEPENDS "${GEN_BRAND_TOOL}"
    COMMENT "Generating Waterscape procedural brand icons & banners"
)
add_custom_target(waterscape_brand_assets ALL
    DEPENDS
        "${WS_BRAND_DIR}/waterscape.ico"
        "${WS_BRAND_DIR}/waterscape_256.png"
        "${WS_BRAND_DIR}/waterscape_48.png"
        "${WS_BRAND_DIR}/waterscape.svg"
        "${WS_BRAND_DIR}/wizard_banner.bmp"
)

set(WS_BUNDLED_ADDON_OUTPUTS "")
foreach(ADDON_ID IN ITEMS waterscape-shield waterscape-dark waterscape-reader)
    set(ADDON_OUT "${WS_ADDONS_DIR}/${ADDON_ID}.wsx")
    file(GLOB ADDON_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/addons/bundled/${ADDON_ID}/*")
    add_custom_command(
        OUTPUT "${ADDON_OUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${WS_ADDONS_DIR}"
        COMMAND "${PACK_WSX_TOOL}" "${CMAKE_CURRENT_SOURCE_DIR}/addons/bundled/${ADDON_ID}" "${ADDON_OUT}"
        DEPENDS "${PACK_WSX_TOOL}" ${ADDON_SRCS}
        COMMENT "Packaging bundled extension ${ADDON_ID}.wsx"
    )
    list(APPEND WS_BUNDLED_ADDON_OUTPUTS "${ADDON_OUT}")
endforeach()
add_custom_target(waterscape_bundled_addons ALL DEPENDS ${WS_BUNDLED_ADDON_OUTPUTS})

# =============================================================================
# Tier 6: Waterscape Browser Shell & Hardware-Accelerated Desktop Presenters
# =============================================================================
add_library(arcr_shell STATIC
    addons/ws_addons.c
    shell/arcr_shell.c
    shell/waterscape_win32.c
    shell/waterscape_x11.c
)
target_link_libraries(arcr_shell PUBLIC arcr_engine arcr_platform_deps)

if((NOT WIN32) AND PkgConfig_FOUND AND WATERSCAPE_ENABLE_X11)
    pkg_check_modules(X11_SHM QUIET x11 xext)
    if(X11_SHM_FOUND)
        target_compile_definitions(arcr_shell PUBLIC WATERSCAPE_HAVE_X11=1)
        target_include_directories(arcr_shell PUBLIC ${X11_SHM_INCLUDE_DIRS})
        target_link_libraries(arcr_shell PUBLIC ${X11_SHM_LIBRARIES})
    endif()
endif()

set(WS_WIN32_RC_SRCS "")
if(WIN32)
    set(WS_WIN32_RC_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/installer/win32/waterscape.rc")
    set_source_files_properties("${WS_WIN32_RC_SRCS}" PROPERTIES
        COMPILE_DEFINITIONS "WATERSCAPE_HAVE_ICON_RC"
        INCLUDE_DIRECTORIES "${WS_BRAND_DIR};${CMAKE_CURRENT_SOURCE_DIR}/installer/win32"
        OBJECT_DEPENDS "${WS_BRAND_DIR}/waterscape.ico;${CMAKE_CURRENT_SOURCE_DIR}/installer/win32/waterscape.manifest"
    )
endif()

# Primary Waterscape Browser Executable (WIN32 GUI on Windows — zero console flash!) + `arcradient` CLI/GUI Binary
add_executable(waterscape WIN32 shell/main.c ${WS_WIN32_RC_SRCS})
target_link_libraries(waterscape PRIVATE arcr_shell)
add_dependencies(waterscape waterscape_brand_assets waterscape_bundled_addons ws_plugin_perf_hud)
if(WIN32)
    target_include_directories(waterscape PRIVATE "${WS_BRAND_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/installer/win32")
endif()

add_executable(arcradient shell/main.c ${WS_WIN32_RC_SRCS})
target_link_libraries(arcradient PRIVATE arcr_shell)
add_dependencies(arcradient waterscape_brand_assets waterscape_bundled_addons ws_plugin_perf_hud)
if(WIN32)
    target_include_directories(arcradient PRIVATE "${WS_BRAND_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/installer/win32")
endif()

# =============================================================================
# Tier 7 (Part B): Native Self-Extracting Setup Wizard & Uninstaller
# =============================================================================
add_executable(waterscape_uninstall
    installer/win32/waterscape_uninstall.c
    base/arcr_str.c
    ${WS_WIN32_RC_SRCS}
)
target_include_directories(waterscape_uninstall PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(waterscape_uninstall PROPERTIES OUTPUT_NAME "uninstall")
add_dependencies(waterscape_uninstall waterscape_brand_assets)
if(WIN32)
    target_include_directories(waterscape_uninstall PRIVATE "${WS_BRAND_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/installer/win32")
    target_link_libraries(waterscape_uninstall PRIVATE user32 advapi32 shell32 ole32)
endif()

add_executable(waterscape_setup_stub
    installer/win32/waterscape_setup.c
    base/arcr_codec.c
    base/arcr_str.c
    base/arcr_cpuid.c
    ${WS_WIN32_RC_SRCS}
)
target_include_directories(waterscape_setup_stub PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_dependencies(waterscape_setup_stub waterscape_brand_assets)
if(WIN32)
    target_include_directories(waterscape_setup_stub PRIVATE "${WS_BRAND_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/installer/win32")
    target_link_libraries(waterscape_setup_stub PRIVATE
        user32 gdi32 advapi32 shell32 ole32 comctl32
    )
endif()

add_executable(win32_ui_verify tools/win32_ui_verify.c)
target_include_directories(win32_ui_verify PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(WIN32)
    target_link_libraries(win32_ui_verify PRIVATE user32 gdi32 ole32)
endif()

if(WIN32)
    # Match arcr_platform_format_module_path ("%s\\%s.dll") without MinGW "lib" prefix
    set_target_properties(
        arcr_engine arcr_net arcr_js_portable arcr_media arcr_devtools ws_plugin_perf_hud
        PROPERTIES
            PREFIX ""
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    set_target_properties(
        waterscape arcradient arcr_js win32_ui_verify waterscape_uninstall waterscape_setup_stub
        PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    set(WS_STAGE_DIR "${CMAKE_BINARY_DIR}/installer_stage")
    set(WS_SETUP_EXE "${CMAKE_BINARY_DIR}/WaterscapeSetup.exe")

    add_custom_command(
        OUTPUT "${WS_SETUP_EXE}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${WS_STAGE_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${WS_STAGE_DIR}/addons"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:waterscape>"           "${WS_STAGE_DIR}/"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:arcradient>"           "${WS_STAGE_DIR}/"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:arcr_js>"              "${WS_STAGE_DIR}/"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:waterscape_uninstall>" "${WS_STAGE_DIR}/"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:arcr_engine>"          "${WS_STAGE_DIR}/"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:arcr_net>"             "${WS_STAGE_DIR}/"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:arcr_js_portable>"     "${WS_STAGE_DIR}/"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:arcr_media>"           "${WS_STAGE_DIR}/"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:arcr_devtools>"        "${WS_STAGE_DIR}/"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:ws_plugin_perf_hud>"   "${WS_STAGE_DIR}/"
        COMMAND "${CMAKE_COMMAND}" -E copy "${WS_BRAND_DIR}/waterscape.ico"      "${WS_STAGE_DIR}/"
        COMMAND "${CMAKE_COMMAND}" -E copy ${WS_BUNDLED_ADDON_OUTPUTS}           "${WS_STAGE_DIR}/addons/"
        COMMAND "${PACK_INSTALLER_TOOL}" "$<TARGET_FILE:waterscape_setup_stub>" "${WS_STAGE_DIR}" "${WS_SETUP_EXE}"
        DEPENDS
            waterscape arcradient arcr_js waterscape_uninstall waterscape_setup_stub
            arcr_engine arcr_net arcr_js_portable arcr_media arcr_devtools ws_plugin_perf_hud
            waterscape_brand_assets waterscape_bundled_addons "${PACK_INSTALLER_TOOL}"
        COMMENT "Packing single-file self-extracting Windows installer WaterscapeSetup.exe"
    )
    add_custom_target(waterscape_win32_installer ALL DEPENDS "${WS_SETUP_EXE}")
endif()

# =============================================================================
# Verification & Benchmark Suite
# =============================================================================
enable_testing()
add_executable(arcr_tests tests/test_suite.c)
target_link_libraries(arcr_tests PRIVATE arcr_shell arcr_engine arcr_platform_deps)
add_dependencies(arcr_tests
    arcr_net arcr_js_portable arcr_js win32_ui_verify arcr_media arcr_devtools ws_plugin_perf_hud
    waterscape_bundled_addons waterscape_brand_assets
    waterscape_setup_stub waterscape_uninstall
)
if(NOT CMAKE_CROSSCOMPILING)
    add_dependencies(arcr_tests pack_installer)
endif()
if(WIN32)
    add_dependencies(arcr_tests waterscape_win32_installer)
    set_target_properties(arcr_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
add_test(NAME WaterscapeFullSuite COMMAND arcr_tests "$<TARGET_FILE_DIR:arcr_net>")
