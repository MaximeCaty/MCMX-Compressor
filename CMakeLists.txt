cmake_minimum_required(VERSION 3.16)
project(mcm CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB_RECURSE SOURCES "source/*.cpp")
list(FILTER SOURCES EXCLUDE REGEX "FilterTest\\.cpp$")

message(STATUS "Sources found:")
foreach(src ${SOURCES})
    message(STATUS "  ${src}")
endforeach()

include_directories(
    source
    source/compressors
)

add_compile_definitions(
    OVERRIDE=override
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    NDEBUG
)

# ── Base options shared by all targets ───────────────────────────────────────
set(MCM_BASE_OPTIONS
    -O3
    -march=native
    -mstackrealign
    -fomit-frame-pointer
    -funroll-loops
    -fno-stack-protector
    -ffast-math
    -w
    -fpermissive
)

set(MCM_LINK_LIBS "")
if(WIN32)
    list(APPEND MCM_LINK_LIBS -static)
endif()

# Profile directory — fixed path so train and use always agree
set(PGO_DIR "${CMAKE_BINARY_DIR}/pgo_profiles")
file(MAKE_DIRECTORY ${PGO_DIR})

# ── Target 1: mcmx — normal release build with LTO ───────────────────────────
add_executable(mcm ${SOURCES})
set_target_properties(mcm PROPERTIES OUTPUT_NAME "mcmx")
target_compile_options(mcm PRIVATE ${MCM_BASE_OPTIONS} -flto=auto)
target_link_options(mcm PRIVATE -flto=auto)
target_link_libraries(mcm PRIVATE ${MCM_LINK_LIBS})

# ── Target 2: mcmx_train — instrumented build for PGO data collection ─────────
# NO -flto here: combining -fprofile-generate with LTO causes missing .gcda files.
add_executable(mcm_train ${SOURCES})
set_target_properties(mcm_train PROPERTIES OUTPUT_NAME "mcmx_train")
target_compile_options(mcm_train PRIVATE
    ${MCM_BASE_OPTIONS}
    -fprofile-generate=${PGO_DIR}
)
target_link_options(mcm_train PRIVATE
    -fprofile-generate=${PGO_DIR}
)
target_link_libraries(mcm_train PRIVATE ${MCM_LINK_LIBS})

# ── Target 3: mcmx_pgo — optimised binary using collected profiles ────────────
# LTO is safe here — profiles are already collected before this build.
add_executable(mcm_pgo ${SOURCES})
set_target_properties(mcm_pgo PROPERTIES OUTPUT_NAME "mcmx_pgo")
target_compile_options(mcm_pgo PRIVATE
    ${MCM_BASE_OPTIONS}
    -flto=auto
    -fprofile-use=${PGO_DIR}
    -fprofile-correction   # tolerates minor source/profile skew
)
target_link_options(mcm_pgo PRIVATE
    -flto=auto
    -fprofile-use=${PGO_DIR}
)
target_link_libraries(mcm_pgo PRIVATE ${MCM_LINK_LIBS})

# ── Target 4: debug build with -O0 to diagnose -x crash ──────────────────────
add_executable(mcm_debug ${SOURCES})
set_target_properties(mcm_debug PROPERTIES OUTPUT_NAME "mcmx_debug")
target_compile_options(mcm_debug PRIVATE
    -O1 -g -w -fpermissive
)
target_compile_definitions(mcm_debug PRIVATE
    OVERRIDE=override NOMINMAX WIN32_LEAN_AND_MEAN NDEBUG
)
target_link_libraries(mcm_debug PRIVATE ${MCM_LINK_LIBS})