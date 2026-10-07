#include "util.hpp"

#include <charconv>
#include <cmath>

namespace semop::detail {

std::string repr(double x) {
  if (std::isnan(x)) return "nan";
  if (std::isinf(x)) return x > 0 ? "inf" : "-inf";
  char buffer[64];
  const auto end = std::to_chars(buffer, buffer + sizeof buffer, x).ptr;
  std::string text(buffer, end);
  if (text.find_first_of(".e") == std::string::npos) text += ".0";
  return text;
}

std::string repr(const Json& value) {
  if (value.is_string()) return "'" + value.get<std::string>() + "'";
  if (value.is_boolean()) return value.get<bool>() ? "True" : "False";
  if (value.is_null()) return "None";
  if (value.is_number_float()) return repr(value.get<double>());
  return value.dump();
}

std::string repr(const AnswerValue& value) {
  if (const auto* b = std::get_if<bool>(&value)) return *b ? "True" : "False";
  if (const auto* s = std::get_if<std::string>(&value)) return "'" + *s + "'";
  if (const auto* d = std::get_if<double>(&value)) return repr(*d);
  return "None";
}

std::string repr(const std::vector<std::string>& items) {
  std::string text = "[";
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) text += ", ";
    text += "'" + items[i] + "'";
  }
  return text + "]";
}

double fsum(const std::vector<double>& values) {
  double sum = 0;
  double compensation = 0;
  for (double v : values) {
    const double t = sum + v;
    if (std::fabs(sum) >= std::fabs(v)) {
      compensation += (sum - t) + v;
    } else {
      compensation += (v - t) + sum;
    }
    sum = t;
  }
  return sum + compensation;
}

bool blank(std::string_view text) {
  for (char c : text) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v') return false;
  }
  return true;
}

Error invalid(ErrorKind kind, std::string message, std::string path) {
  Error error;
  error.kind = kind;
  error.message = std::move(message);
  error.path = std::move(path);
  return error;
}

}  // namespace semop::detail
