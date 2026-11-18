# Script mode: turns a SPIR-V binary into a C++ header holding a byte array.
#   cmake -DINPUT=<file.spv> -DOUTPUT=<file.h> -DSYMBOL=<name> -P EmbedSpirv.cmake
if(NOT INPUT OR NOT OUTPUT OR NOT SYMBOL)
    message(FATAL_ERROR "EmbedSpirv.cmake needs INPUT, OUTPUT and SYMBOL")
endif()

file(READ "${INPUT}" _hex HEX)
string(LENGTH "${_hex}" _hexLength)
math(EXPR _byteCount "${_hexLength} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
# Wrap lines so the generated header stays readable and compiler friendly.
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n    " _bytes "${_bytes}")

file(WRITE "${OUTPUT}"
"// Generated from ${INPUT} - do not edit.
#pragma once
#include <cstddef>

namespace se::render::shaders {
// SPIR-V is a stream of 32-bit words, hence the alignment.
alignas(4) inline constexpr unsigned char ${SYMBOL}[] = {
    ${_bytes}
};
inline constexpr std::size_t ${SYMBOL}Size = ${_byteCount};
} // namespace se::render::shaders
")
