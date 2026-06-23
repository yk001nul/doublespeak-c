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

# ── FetchContent: cJSON ───────────────────────────────────────────────────────

include(FetchContent)

FetchContent_Declare(cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG        v1.7.18
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(cjson)

# ── System dependencies ───────────────────────────────────────────────────────

find_package(CURL REQUIRED)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SODIUM libsodium)
endif()
if(NOT SODIUM_FOUND)
    find_library(SODIUM_LIBRARIES NAMES sodium libsodium REQUIRED)
    find_path(SODIUM_INCLUDE_DIRS sodium.h PATH_SUFFIXES sodium)
    if(NOT SODIUM_INCLUDE_DIRS)
        message(FATAL_ERROR "libsodium headers not found. Install libsodium-dev or set SODIUM_INCLUDE_DIRS.")
    endif()
endif()

if(METEOR_USE_LIBHYPHEN)
    find_library(HYPHEN_LIB NAMES hyphen REQUIRED)
    if(NOT HYPHEN_LIB)
        message(FATAL_ERROR "libhyphen not found. Install libhyphen-dev or disable METEOR_USE_LIBHYPHEN.")
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
    PRIVATE ${SODIUM_INCLUDE_DIRS}
            ${cjson_SOURCE_DIR}
)

target_link_libraries(meteor PRIVATE
    CURL::libcurl
    cjson
    ${SODIUM_LIBRARIES}
)

if(SODIUM_CFLAGS_OTHER)
    target_compile_options(meteor PRIVATE ${SODIUM_CFLAGS_OTHER})
endif()

if(METEOR_USE_LIBHYPHEN)
    target_compile_definitions(meteor PRIVATE METEOR_USE_LIBHYPHEN)
    target_link_libraries(meteor PRIVATE ${HYPHEN_LIB})
endif()

if(WIN32)
    target_compile_definitions(meteor PRIVATE METEOR_BUILDING_DLL)
    set_target_properties(meteor PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
else()
    set_target_properties(meteor PROPERTIES C_VISIBILITY_PRESET hidden)
    target_compile_definitions(meteor PRIVATE METEOR_BUILDING_DLL)
endif()

# link math for roundf in meteor_core
if(UNIX)
    target_link_libraries(meteor PRIVATE m)
endif()

# ── llama.cpp server (external project) ───────────────────────────────────────

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
    EXCLUDE_FROM_ALL TRUE   # do not build automatically; use: cmake --build . --target llama_server_build
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
    COMMAND ${CMAKE_COMMAND} -E echo "Checking llama-server health on port ${METEOR_LLM_PORT}..."
    COMMAND curl --silent --fail --max-time 3
            "http://127.0.0.1:${METEOR_LLM_PORT}/health"
    COMMENT "Health check"
    USES_TERMINAL
)

# ── Tests ─────────────────────────────────────────────────────────────────────

enable_testing()

foreach(test prng bits meteor_core syllabifier)
    add_executable(test_${test} tests/test_${test}.c)
    target_include_directories(test_${test} PRIVATE ${SODIUM_INCLUDE_DIRS})
    target_link_libraries(test_${test} PRIVATE meteor ${SODIUM_LIBRARIES})
    if(UNIX)
        target_link_libraries(test_${test} PRIVATE m)
    endif()
    add_test(NAME ${test} COMMAND test_${test})
endforeach()

foreach(test roundtrip determinism)
    add_executable(test_${test} tests/test_${test}.c)
    target_include_directories(test_${test} PRIVATE ${SODIUM_INCLUDE_DIRS})
    target_link_libraries(test_${test} PRIVATE meteor ${SODIUM_LIBRARIES})
    if(UNIX)
        target_link_libraries(test_${test} PRIVATE m)
    endif()
    add_test(NAME ${test} COMMAND test_${test})
    set_tests_properties(${test} PROPERTIES
        SKIP_REGULAR_EXPRESSION "llama-server not reachable"
    )
endforeach()
