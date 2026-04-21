cmake_minimum_required(VERSION 3.20)
project(FluxHLS CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Locate libclang ────────────────────────────────────────────────────────────
# Search priority:
#   1. Explicit -DLLVM_INSTALL_DIR=<path>  (user override, any platform)
#   2. llvm-config / llvm-config-XX        (auto-detect from PATH)
#   3. Common versioned Linux paths         (/usr/lib/llvm-18/lib, etc.)
#   4. Common unversioned Linux/macOS paths (/usr/lib, /usr/local/lib, Homebrew)
#   5. Default Windows install              (C:/Program Files/LLVM)

set(_lib_hints "")
set(_inc_hints "")

# 1. User-supplied explicit root
if(DEFINED LLVM_INSTALL_DIR)
    list(APPEND _lib_hints "${LLVM_INSTALL_DIR}/lib")
    list(APPEND _inc_hints "${LLVM_INSTALL_DIR}/include")
endif()

# 2. llvm-config (works on Linux, macOS, and Windows if in PATH)
find_program(_llvm_config
    NAMES llvm-config llvm-config-18 llvm-config-17 llvm-config-16 llvm-config-15)
if(_llvm_config)
    execute_process(COMMAND ${_llvm_config} --libdir
        OUTPUT_VARIABLE _lc_lib OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${_llvm_config} --includedir
        OUTPUT_VARIABLE _lc_inc OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_lc_lib)
        list(APPEND _lib_hints "${_lc_lib}")
    endif()
    if(_lc_inc)
        list(APPEND _inc_hints "${_lc_inc}")
    endif()
endif()

# 3. Versioned Ubuntu/Debian paths
foreach(_ver 18 17 16 15 14)
    list(APPEND _lib_hints "/usr/lib/llvm-${_ver}/lib")
    list(APPEND _inc_hints "/usr/lib/llvm-${_ver}/include")
endforeach()

# 4. Unversioned Linux / macOS (Homebrew)
list(APPEND _lib_hints
    /usr/lib /usr/local/lib
    /usr/lib/x86_64-linux-gnu
    /usr/lib/aarch64-linux-gnu
    /opt/homebrew/opt/llvm/lib      # macOS Apple Silicon
    /usr/local/opt/llvm/lib)        # macOS Intel Homebrew

list(APPEND _inc_hints
    /usr/include /usr/local/include
    /opt/homebrew/opt/llvm/include
    /usr/local/opt/llvm/include)

# 5. Windows default
if(WIN32)
    list(APPEND _lib_hints "C:/Program Files/LLVM/lib")
    list(APPEND _inc_hints "C:/Program Files/LLVM/include")
endif()

find_library(LIBCLANG_LIB
    NAMES libclang clang
    HINTS ${_lib_hints}
    REQUIRED
)

find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h
    HINTS ${_inc_hints}
    REQUIRED
)

message(STATUS "[FluxHLS] libclang  : ${LIBCLANG_LIB}")
message(STATUS "[FluxHLS] clang-c/  : ${LIBCLANG_INCLUDE_DIR}")

# ── Build target ───────────────────────────────────────────────────────────────

add_executable(fluxhls
    main.cpp
    stage1/Frontend.cpp
    stage1/Dumper.cpp
)

target_include_directories(fluxhls PRIVATE
    "${LIBCLANG_INCLUDE_DIR}"
    "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(fluxhls ${LIBCLANG_LIB})

# Linux: libclang.so sometimes needs libdl at link time
if(UNIX AND NOT APPLE)
    target_link_libraries(fluxhls dl)
endif()

# Windows: copy libclang.dll next to the exe so it is found at runtime.
# Derive the bin/ dir from the lib/ dir (standard LLVM layout).
if(WIN32)
    get_filename_component(_libclang_libdir "${LIBCLANG_LIB}" DIRECTORY)
    string(REGEX REPLACE "[/\\\\]lib$" "/bin" _libclang_bindir "${_libclang_libdir}")
    if(EXISTS "${_libclang_bindir}/libclang.dll")
        add_custom_command(TARGET fluxhls POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_libclang_bindir}/libclang.dll"
                $<TARGET_FILE_DIR:fluxhls>
            COMMENT "Copying libclang.dll"
        )
    endif()
endif()
