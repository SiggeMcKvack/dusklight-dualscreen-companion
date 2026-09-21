cmake_minimum_required(VERSION 3.25)
project(dualscreen_companion CXX)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(DUSK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/dusklight" CACHE PATH "Path to the Dusklight source root")
    option(DUSK_MOD_USE_FULL_TREE "Use full build instead of the minimal mod SDK" OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    if (DUSK_MOD_USE_FULL_TREE)
        add_subdirectory("${DUSK_DIR}" dusk EXCLUDE_FROM_ALL)
    else ()
        add_subdirectory("${DUSK_DIR}/sdk" dusk-sdk EXCLUDE_FROM_ALL)
    endif ()
endif ()

if (NOT ANDROID)
    message(STATUS "dualscreen_companion: Android only, skipping")
    return()
endif ()

# --- Android SDK tools (javac + d8 + android.jar) for the companion's Java classes ---
set(_sdk "$ENV{ANDROID_HOME}")
if (NOT _sdk)
    set(_sdk "$ENV{ANDROID_SDK_ROOT}")
endif ()
if (NOT _sdk AND CMAKE_ANDROID_NDK)
    get_filename_component(_sdk "${CMAKE_ANDROID_NDK}/../.." ABSOLUTE)
endif ()
file(GLOB _build_tools LIST_DIRECTORIES true "${_sdk}/build-tools/*")
list(SORT _build_tools COMPARE NATURAL)
list(GET _build_tools -1 _build_tools_dir)
file(GLOB _platforms LIST_DIRECTORIES true "${_sdk}/platforms/android-*")
list(SORT _platforms COMPARE NATURAL)
list(GET _platforms -1 _platform_dir)
set(_android_jar "${_platform_dir}/android.jar")
if (NOT EXISTS "${_android_jar}")
    message(FATAL_ERROR "dualscreen_companion: android.jar not found under ${_sdk}/platforms (set ANDROID_HOME)")
endif ()
find_program(DSC_JAVAC javac HINTS "$ENV{JAVA_HOME}/bin" REQUIRED)
find_program(DSC_D8 NAMES d8 d8.bat HINTS "${_build_tools_dir}" REQUIRED)
find_program(DSC_JAR jar HINTS "$ENV{JAVA_HOME}/bin" REQUIRED)

file(GLOB _java_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/java/dev/siggemckvack/dualscreen/*.java")
set(_classes_dir "${CMAKE_CURRENT_BINARY_DIR}/classes")
set(_dex_dir "${CMAKE_CURRENT_BINARY_DIR}/dex")
set(_dex_cpp "${CMAKE_CURRENT_BINARY_DIR}/companion_dex.cpp")

add_custom_command(
    OUTPUT "${_dex_dir}/classes.dex"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_classes_dir}" "${_dex_dir}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_classes_dir}" "${_dex_dir}"
    COMMAND "${DSC_JAVAC}" -source 17 -target 17 -Xlint:-options
            -classpath "${_android_jar}" -d "${_classes_dir}" ${_java_sources}
    COMMAND "${DSC_JAR}" cf "${_classes_dir}.jar" -C "${_classes_dir}" .
    COMMAND "${DSC_D8}" --min-api 28 --lib "${_android_jar}" --output "${_dex_dir}"
            "${_classes_dir}.jar"
    DEPENDS ${_java_sources}
    COMMENT "dualscreen_companion: compiling Java -> DEX"
    VERBATIM)

add_custom_command(
    OUTPUT "${_dex_cpp}"
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${_dex_dir}/classes.dex" "-DOUTPUT=${_dex_cpp}" "-DSYMBOL=g_dualScreenDex"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBinary.cmake"
    DEPENDS "${_dex_dir}/classes.dex" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBinary.cmake"
    COMMENT "dualscreen_companion: embedding DEX"
    VERBATIM)

add_mod(dev.siggemckvack.dualscreen_companion
    FEATURES fmt game webgpu
    SOURCES
        src/mod.cpp
        src/jni_bridge.cpp
        src/present.cpp
        src/game_access.cpp
        src/hooks.cpp
        src/config.cpp
        src/slots.cpp
        src/companion/dualscreen.cpp
        src/companion/companion.cpp
        src/companion/companion_collect.cpp
        src/companion/companion_dmap.cpp
        src/companion/companion_functional.cpp
        src/companion/companion_gfx.cpp
        src/companion/companion_guide.cpp
        src/guide/browser.cpp
        src/guide/convert.cpp
        src/guide/fetch.cpp
        src/guide/image.cpp
        src/guide/store.cpp
        src/companion/companion_hud.cpp
        src/companion/companion_icons.cpp
        src/companion/companion_pages.cpp
        src/companion/companion_strings.cpp
        src/companion/companion_touch.cpp
        "${_dex_cpp}"
    MOD_JSON mod.json
)
target_include_directories(dev.siggemckvack.dualscreen_companion PRIVATE src/include)

# The guide store's index is JSON (header-only nlohmann_json, same version Dusklight uses).
if (NOT TARGET nlohmann_json::nlohmann_json)
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
            URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
            URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_MakeAvailable(nlohmann_json)
endif ()
target_link_libraries(dev.siggemckvack.dualscreen_companion PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(dev.siggemckvack.dualscreen_companion PRIVATE android log)
