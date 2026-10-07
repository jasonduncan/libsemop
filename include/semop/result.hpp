#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

/// Checks preconditions such as `Result::value()` on an error. Define before including to route
/// failures to a host application's own check macro.
#ifndef SEMOP_ASSERT
#include <cassert>
#define SEMOP_ASSERT(condition, message) assert((condition) && (message))
#endif

namespace semop {

/// JSON for state and raw provider answers. Ordered, so keys keep the order they were written in.
/// The same type as `libtypesafe::Json`.
using Json = nlohmann::ordered_json;

enum class ErrorKind {
  invalid_question,  ///< A malformed question or question set; nothing was sent.
  invalid_argument,  ///< Another bad argument (concurrency, weights, a blocking call in a callback).
  invalid_request,   ///< A `semop` JSON request that doesn't match the wire format.
  provider_error,    ///< The provider failed or returned something malformed.
  timeout,           ///< The provider ran out of time. Also a provider error.
};

[[nodiscard]] std::string_view to_string(ErrorKind kind) noexcept;

/// Every failure libsemop reports. The library never throws.
struct Error {
  ErrorKind kind = ErrorKind::invalid_argument;
  /// Full description; provider errors start with the provider's name, e.g. "TypeSafe: ...".
  std::string message;
  /// provider_error / timeout: the provider that failed.
  std::string provider;
  /// invalid_question / invalid_request: where, e.g. `questions[1].options`.
  std::string path;

  /// A provider error or timeout, like Python's `ProviderError`.
  [[nodiscard]] bool is_provider_error() const noexcept {
    return kind == ErrorKind::provider_error || kind == ErrorKind::timeout;
  }
};

/// A value or an Error. Accessing the wrong side is a precondition failure (SEMOP_ASSERT).
template <class T>
class [[nodiscard]] Result {
 public:
  Result(T value) : v_(std::in_place_index<0>, std::move(value)) {}
  Result(Error error) : v_(std::in_place_index<1>, std::move(error)) {}

  [[nodiscard]] bool ok() const noexcept { return v_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] T& value() & { return *checked(); }
  [[nodiscard]] const T& value() const& { return *checked(); }
  [[nodiscard]] T&& value() && { return std::move(*checked()); }
  T* operator->() { return checked(); }
  const T* operator->() const { return checked(); }
  T& operator*() & { return *checked(); }
  const T& operator*() const& { return *checked(); }

  [[nodiscard]] const Error& error() const& {
    SEMOP_ASSERT(!ok(), "Result::error() called on a value");
    return *std::get_if<1>(&v_);
  }

 private:
  T* checked() {
    SEMOP_ASSERT(ok(), "Result::value() called on an error");
    return std::get_if<0>(&v_);
  }
  const T* checked() const {
    SEMOP_ASSERT(ok(), "Result::value() called on an error");
    return std::get_if<0>(&v_);
  }

  std::variant<T, Error> v_;
};

}  // namespace semop
