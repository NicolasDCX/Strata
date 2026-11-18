#pragma once

#include <expected>
#include <string>
#include <utility>

namespace se::core {

/// Recoverable failure description. Programming errors use SE_ASSERT instead.
struct Error {
    std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

inline std::unexpected<Error> makeError(std::string message)
{
    return std::unexpected<Error>(Error{std::move(message)});
}

} // namespace se::core
