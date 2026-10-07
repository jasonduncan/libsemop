#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "semop/answers.hpp"
#include "semop/questions.hpp"
#include "semop/result.hpp"

namespace semop::wire {

// The JSON request and response shared with the Python `semop` CLI and MCP server
// (schema_version "1"). A request:
//
//   {"state": "...", "questions": [
//      {"name": "is_complaint", "type": "boolean", "instructions": "...", "min_confidence": 0.8,
//       "true": "...", "false": "..."},
//      {"name": "department", "type": "choice", "instructions": "...",
//       "options": [{"name": "billing", "description": "..."}, "technical"]},
//      {"name": "urgency", "type": "score", "instructions": "...", "levels": ["low", "high"]}],
//    "request_id": "ticket-42", "schema_version": "1"}
//
// Unknown fields are rejected, so typos fail loudly; errors name the path. A response has
// `answers` by name (type, value or null, decided, confidence, probabilities in question
// order), one `call`, and the echoed request_id. Numbers are rounded to 4 significant digits.

inline constexpr int significant_digits = 4;

struct Request {
  Json state;
  Questions questions;
  std::optional<std::string> request_id;
};

/// Decode a parsed JSON request. Errors are `invalid_request` with `path` set, and messages
/// that match the Python implementation's.
[[nodiscard]] Result<Request> decode(const Json& request);

/// The JSON response for answers from one call.
[[nodiscard]] Json encode(const Answers& answers, const Questions& questions,
                          const std::optional<std::string>& request_id = std::nullopt);

/// A failure response. `code`: invalid_request, provider_error, or timeout.
[[nodiscard]] Json error(std::string_view code, std::string_view message,
                         const std::optional<std::string>& request_id = std::nullopt);

/// The wire code for an error: invalid_request, provider_error, or timeout.
[[nodiscard]] std::string_view code(const Error& error) noexcept;

/// `x` rounded to 4 significant digits, as Python's `float(f"{x:.4g}")`.
[[nodiscard]] double significant(double x);

}  // namespace semop::wire
