#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "semop/answers.hpp"
#include "semop/result.hpp"

namespace semop::detail {

/// Python's repr() of a float: shortest round-trip digits, with ".0" for whole numbers.
std::string repr(double x);
/// Python's repr() of a JSON value as Python would hold it: 'text', True, None, 3, 0.5.
std::string repr(const Json& value);
/// Python's repr() of an answer value.
std::string repr(const AnswerValue& value);
/// Python's repr() of a list of strings: ['a', 'b'].
std::string repr(const std::vector<std::string>& items);

/// Accurate sum (Neumaier), close to Python's math.fsum for short lists.
double fsum(const std::vector<double>& values);

/// Whitespace-only or empty.
bool blank(std::string_view text);

Error invalid(ErrorKind kind, std::string message, std::string path = {});

}  // namespace semop::detail
