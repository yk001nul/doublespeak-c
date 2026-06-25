cmake_minimum_required(VERSION 3.18)
project(meteor_stego C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── User-configurable options ─────────────────────────────────────────────────

set(LLAMA_CPP_GIT_TAG "b4693"
    CACHE STRING "llama.cpp git tag or commit hash to build llama-server from")

set(LLAMA_CPP_REPO "https://github.com/ggerganov/llama.cpp.git"
    CACHE STRING "llama.cpp repository URL")

set(METEOR_LLM_PORT "8080"
    CACHE STRING "Port llama-server listens on")

set(METEOR_LLM_SEED "42"
    CACHE STRING "RNG seed passed to llama-server (must match on encoder and decoder)")

set(METEOR_MODEL_PATH ""
    CACHE FILEPATH "Path to the GGUF model file")

option(METEOR_USE_LIBHYPHEN "Link against libhyphen for accurate syllabification" OFF)

# ── Vendored cJSON ────────────────────────────────────────────────────────────
# cJSON.c / cJSON.h are vendored in third_party/cjson/ (MIT licence, v1.7.18).
# We build it ourselves so we control compiler flags and avoid /Za incompatibility.

add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cjson)
# Silence MSVC warnings from cJSON source
if(MSVC)
    target_compile_options(cjson PRIVATE /W0)
endif()
set(cjson_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/cjson")

# ── libsodium ─────────────────────────────────────────────────────────────────

if(WIN32)
    # Use the prebuilt MSVC binaries shipped in deps/libsodium/
    # Layout: deps/libsodium/libsodium/x64/<Config>/v143/static/libsodium.lib
    set(SODIUM_PREBUILT_ROOT "${CMAKE_SOURCE_DIR}/deps/libsodium/libsodium")

    set(SODIUM_INCLUDE_DIRS "${SODIUM_PREBUILT_ROOT}/include")

    # Generator expression selects Debug or Release lib automatically
    set(SODIUM_DEBUG_LIB
        "${SODIUM_PREBUILT_ROOT}/x64/Debug/v143/static/libsodium.lib")
    set(SODIUM_RELEASE_LIB
        "${SODIUM_PREBUILT_ROOT}/x64/Release/v143/static/libsodium.lib")

    if(NOT EXISTS "${SODIUM_DEBUG_LIB}" AND NOT EXISTS "${SODIUM_RELEASE_LIB}")
        message(FATAL_ERROR
            "Prebuilt libsodium not found at ${SODIUM_PREBUILT_ROOT}.\n"
            "Extract deps/libsodium-msvc.zip into deps/libsodium/ first.")
    endif()

    # Use an IMPORTED target so generator expressions work per-config
    add_library(sodium_prebuilt STATIC IMPORTED)
    set_target_properties(sodium_prebuilt PROPERTIES
        IMPORTED_LOCATION_DEBUG   "${SODIUM_DEBUG_LIB}"
        IMPORTED_LOCATION_RELEASE "${SODIUM_RELEASE_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${SODIUM_INCLUDE_DIRS}"
    )
    # Fall back: if only one config is built, use whichever exists
    if(NOT EXISTS "${SODIUM_DEBUG_LIB}")
        set_target_properties(sodium_prebuilt PROPERTIES IMPORTED_LOCATION "${SODIUM_RELEASE_LIB}")
    elseif(NOT EXISTS "${SODIUM_RELEASE_LIB}")
        set_target_properties(sodium_prebuilt PROPERTIES IMPORTED_LOCATION "${SODIUM_DEBUG_LIB}")
    else()
        set_target_properties(sodium_prebuilt PROPERTIES IMPORTED_LOCATION "${SODIUM_DEBUG_LIB}")
    endif()

    set(SODIUM_TARGET sodium_prebuilt)
    # Static libsodium on Windows needs bcrypt for RNG
    set(SODIUM_EXTRA_LIBS bcrypt)
    set(SODIUM_COMPILE_DEFS SODIUM_STATIC)

else()
    # Linux / macOS: use pkg-config or find_library
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SODIUM libsodium)
    endif()
    if(NOT SODIUM_FOUND)
        find_library(SODIUM_LIB NAMES sodium libsodium REQUIRED)
        find_path(SODIUM_INC sodium.h PATH_SUFFIXES sodium)
        if(NOT SODIUM_INC)
            message(FATAL_ERROR "libsodium headers not found.")
        endif()
        set(SODIUM_LIBRARIES ${SODIUM_LIB})
        set(SODIUM_INCLUDE_DIRS ${SODIUM_INC})
    endif()
    add_library(sodium_prebuilt INTERFACE IMPORTED)
    target_link_libraries(sodium_prebuilt INTERFACE ${SODIUM_LIBRARIES})
    target_include_directories(sodium_prebuilt INTERFACE ${SODIUM_INCLUDE_DIRS})
    set(SODIUM_TARGET sodium_prebuilt)
    set(SODIUM_EXTRA_LIBS "")
    set(SODIUM_COMPILE_DEFS "")
endif()

# ── HTTP library ──────────────────────────────────────────────────────────────

if(WIN32)
    # Use Windows-native WinHTTP — no external dependency needed
    set(HTTP_COMPILE_DEF METEOR_HTTP_WINHTTP)
    set(HTTP_LINK_LIBS   winhttp ws2_32)
    message(STATUS "HTTP backend: WinHTTP (Windows native)")
else()
    find_package(CURL REQUIRED)
    set(HTTP_COMPILE_DEF "")
    set(HTTP_LINK_LIBS   CURL::libcurl)
    message(STATUS "HTTP backend: libcurl")
endif()

# ── libhyphen (optional) ──────────────────────────────────────────────────────

if(METEOR_USE_LIBHYPHEN)
    find_library(HYPHEN_LIB NAMES hyphen REQUIRED)
    if(NOT HYPHEN_LIB)
        message(FATAL_ERROR "libhyphen not found. Disable METEOR_USE_LIBHYPHEN to use the heuristic.")
    endif()
endif()

# ── Meteor library ────────────────────────────────────────────────────────────

add_library(meteor SHARED
    src/meteor.c
    src/prng.c
    src/bits.c
    src/meteor_core.c
    src/syllabifier.c
    src/llm_client.c
    src/encode.c
    src/decode.c
)

target_include_directories(meteor
    PUBLIC  include
    PRIVATE ${cjson_SOURCE_DIR}
)

target_compile_definitions(meteor PRIVATE
    ${HTTP_COMPILE_DEF}
    ${SODIUM_COMPILE_DEFS}
)

target_link_libraries(meteor PRIVATE
    ${SODIUM_TARGET}
    ${SODIUM_EXTRA_LIBS}
    ${HTTP_LINK_LIBS}
    cjson
)

if(METEOR_USE_LIBHYPHEN)
    target_compile_definitions(meteor PRIVATE METEOR_USE_LIBHYPHEN)
    target_link_libraries(meteor PRIVATE ${HYPHEN_LIB})
endif()

if(WIN32)
    set_target_properties(meteor PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
else()
    set_target_properties(meteor PROPERTIES C_VISIBILITY_PRESET hidden)
    target_link_libraries(meteor PRIVATE m)
endif()

# ── llama.cpp server (external project, opt-in) ───────────────────────────────

include(ExternalProject)

ExternalProject_Add(llama_server_build
    GIT_REPOSITORY   ${LLAMA_CPP_REPO}
    GIT_TAG          ${LLAMA_CPP_GIT_TAG}
    GIT_SHALLOW      TRUE
    GIT_PROGRESS     TRUE
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        -DLLAMA_BUILD_SERVER=ON
        -DLLAMA_BUILD_TESTS=OFF
        -DLLAMA_BUILD_EXAMPLES=OFF
        -DLLAMA_METAL=OFF
        -DLLAMA_CUBLAS=OFF
        -DLLAMA_CLBLAST=OFF
        -DLLAMA_HIPBLAS=OFF
    INSTALL_COMMAND  ""
    EXCLUDE_FROM_ALL TRUE
)

ExternalProject_Get_Property(llama_server_build BINARY_DIR)

set(LLAMA_SERVER_EXE "${CMAKE_BINARY_DIR}/bin/llama-server${CMAKE_EXECUTABLE_SUFFIX}")

add_custom_command(
    OUTPUT  ${LLAMA_SERVER_EXE}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${BINARY_DIR}/bin/llama-server${CMAKE_EXECUTABLE_SUFFIX}"
            "${LLAMA_SERVER_EXE}"
    DEPENDS llama_server_build
    COMMENT "Copying llama-server to build/bin/"
)
add_custom_target(copy_llama_server DEPENDS ${LLAMA_SERVER_EXE})

# ── Start-script generation ───────────────────────────────────────────────────

if(WIN32)
    configure_file(
        "${CMAKE_SOURCE_DIR}/scripts/start_llama_server.bat.in"
        "${CMAKE_BINARY_DIR}/start_llama_server.bat"
        @ONLY
    )
else()
    configure_file(
        "${CMAKE_SOURCE_DIR}/scripts/start_llama_server.sh.in"
        "${CMAKE_BINARY_DIR}/start_llama_server.sh"
        @ONLY
    )
    file(CHMOD "${CMAKE_BINARY_DIR}/start_llama_server.sh"
         PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                     GROUP_READ GROUP_EXECUTE
                     WORLD_READ WORLD_EXECUTE)
endif()

# ── Helper targets ────────────────────────────────────────────────────────────

add_custom_target(server_health
    COMMAND ${CMAKE_COMMAND} -E echo "Checking llama-server on port ${METEOR_LLM_PORT}..."
    COMMAND curl --silent --fail --max-time 3
            "http://127.0.0.1:${METEOR_LLM_PORT}/health"
    COMMENT "Health check"
    USES_TERMINAL
)

# ── Tests ─────────────────────────────────────────────────────────────────────

enable_testing()

set(ALL_TESTS prng bits meteor_core syllabifier roundtrip determinism)

foreach(test ${ALL_TESTS})
    add_executable(test_${test} tests/test_${test}.c)
    target_compile_definitions(test_${test} PRIVATE ${SODIUM_COMPILE_DEFS})
    target_link_libraries(test_${test} PRIVATE
        meteor
        ${SODIUM_TARGET}
        ${SODIUM_EXTRA_LIBS}
        cjson
    )
    add_test(NAME ${test} COMMAND test_${test})
endforeach()

set_tests_properties(roundtrip determinism PROPERTIES
    SKIP_REGULAR_EXPRESSION "llama-server not reachable"
)
