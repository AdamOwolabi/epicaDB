# Some macOS Command Line Tools installs ship an almost-empty
# /Library/Developer/CommandLineTools/usr/include/c++/v1 (only __cxx_version).
# clang searches that directory first and then does NOT fall back to the
# complete copy inside the SDK, so every <cstddef>/<string> include fails.
#
# Detect that situation and point the compiler at the SDK's libc++ headers.
# On healthy machines this module is a no-op.

if(NOT APPLE)
  return()
endif()

include(CheckCXXSourceCompiles)
check_cxx_source_compiles("#include <cstddef>\n#include <string>\nint main(){std::string s;return (int)s.size();}"
                          EPICA_LIBCXX_HEADERS_OK)
if(EPICA_LIBCXX_HEADERS_OK)
  return()
endif()

execute_process(COMMAND xcrun --show-sdk-path
                OUTPUT_VARIABLE _epica_sdk OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _epica_sdk_rc)
if(NOT _epica_sdk_rc EQUAL 0 OR NOT EXISTS "${_epica_sdk}/usr/include/c++/v1/cstddef")
  message(FATAL_ERROR
    "libc++ headers are not usable and no SDK fallback was found. "
    "Try: xcode-select --install, or reinstall Command Line Tools.")
endif()

message(STATUS "Toolchain libc++ headers are broken; using ${_epica_sdk}/usr/include/c++/v1")
add_compile_options(-nostdinc++ "-isystem${_epica_sdk}/usr/include/c++/v1")
