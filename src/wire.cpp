#include "semop/wire.hpp"

#include <charconv>
#include <cmath>
#include <set>

#include "util.hpp"

namespace semop::wire {

namespace {

Error wire_error(const std::string& path, const std::string& message) {
  return detail::invalid(ErrorKind::invalid_request, path.empty() ? message : path + ": " + message, path);
}

/// The first unknown field, alphabetically, as Python's `sorted(set(obj) - allowed)[0]`.
std::optional<Error> no_unknown(const Json& object, const std::set<std::string>& allowed, const std::string& path) {
  std::set<std::string> unknown;
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (allowed.count(it.key()) == 0) unknown.insert(it.key());
  }
  if (unknown.empty()) return std::nullopt;
  return wire_error(path.empty() ? *unknown.begin() : path + "." + *unknown.begin(), "unknown field");
}

const Json* member(const Json& object, const char* key) {
  const auto it = object.find(key);
  return it == object.end() ? nullptr : &*it;
}

bool is_number(const Json& value) { return value.is_number(); }  // JSON booleans aren't numbers

/// `instructions` and `min_confidence` as the Python constructors check them.
std::optional<std::string> common_problem(const Json* instructions, const Json* min_confidence) {
  if (instructions == nullptr || !instructions->is_string() || detail::blank(instructions->get<std::string>())) {
    return "instructions must be a non-empty string";
  }
  if (min_confidence != nullptr) {
    const bool ok = is_number(*min_confidence) && std::isfinite(min_confidence->get<double>()) &&
                    min_confidence->get<double>() >= 0 && min_confidence->get<double>() <= 1;
    if (!ok) return "min_confidence must be a number from 0 to 1, got " + detail::repr(*min_confidence);
  }
  return std::nullopt;
}

std::optional<std::string> labels_problem(const std::vector<Json>& labels, const char* what) {
  if (labels.size() < 2) return "need at least 2 " + std::string(what) + ", got " + std::to_string(labels.size());
  std::set<std::string> seen;
  for (const auto& label : labels) {
    if (!label.is_string() || label.get<std::string>().empty()) return std::string(what) + " must be non-empty strings";
  }
  for (const auto& label : labels) {
    if (!seen.insert(label.get<std::string>()).second) return std::string(what) + " must be unique";
  }
  return std::nullopt;
}

Result<NamedQuestion> decode_question(const Json& spec, const std::string& path) {
  static const std::set<std::string> boolean_fields{"name", "type", "instructions", "min_confidence", "true", "false"};
  static const std::set<std::string> choice_fields{"name", "type", "instructions", "min_confidence", "options"};
  static const std::set<std::string> score_fields{"name", "type", "instructions", "min_confidence", "levels"};

  if (!spec.is_object()) return wire_error(path, "must be a JSON object");
  const Json* type = member(spec, "type");
  const std::string kind = type != nullptr && type->is_string() ? type->get<std::string>() : "";
  if (kind != "boolean" && kind != "choice" && kind != "score") {
    return wire_error(path + ".type", "must be 'boolean', 'choice', or 'score'");
  }
  if (auto unknown = no_unknown(spec, kind == "boolean" ? boolean_fields : kind == "choice" ? choice_fields : score_fields,
                                path)) {
    return *unknown;
  }
  const Json* name = member(spec, "name");
  if (name == nullptr || !name->is_string() || name->get<std::string>().empty()) {
    return wire_error(path + ".name", "must be a non-empty string");
  }
  const Json* instructions = member(spec, "instructions");
  const Json* min_confidence = member(spec, "min_confidence");
  const double confidence = min_confidence != nullptr && is_number(*min_confidence) ? min_confidence->get<double>() : 0;
  const std::string text = instructions != nullptr && instructions->is_string() ? instructions->get<std::string>() : "";

  if (kind == "boolean") {
    Boolean q{text, std::nullopt, std::nullopt, confidence};
    for (const char* side : {"true", "false"}) {
      const Json* value = member(spec, side);
      if (value != nullptr && !value->is_null() && !value->is_string()) {
        return wire_error(path + "." + side, "must be a string");
      }
      if (value != nullptr && value->is_string()) (std::string(side) == "true" ? q.yes : q.no) = value->get<std::string>();
    }
    if (auto problem = common_problem(instructions, min_confidence)) return wire_error(path, *problem);
    return NamedQuestion{name->get<std::string>(), q};
  }

  if (kind == "choice") {
    const Json* options = member(spec, "options");
    if (options == nullptr || !options->is_array()) return wire_error(path + ".options", "must be an array");
    Choice q{text, {}, confidence};
    std::set<std::string> seen;
    for (std::size_t i = 0; i < options->size(); ++i) {
      const Json& option = (*options)[i];
      const std::string where = path + ".options[" + std::to_string(i) + "]";
      const Json* option_name = nullptr;
      std::optional<std::string> description;
      if (option.is_string()) {
        option_name = &option;
      } else if (option.is_object()) {
        if (auto unknown = no_unknown(option, {"name", "description"}, where)) return *unknown;
        option_name = member(option, "name");
        const Json* d = member(option, "description");
        if (d != nullptr && !d->is_null() && !d->is_string()) return wire_error(where + ".description", "must be a string");
        if (d != nullptr && d->is_string()) description = d->get<std::string>();
      } else {
        return wire_error(where, "must be a name or {\"name\", \"description\"}");
      }
      if (option_name == nullptr || !option_name->is_string() || option_name->get<std::string>().empty()) {
        return wire_error(where, "needs a non-empty name");
      }
      const std::string label = option_name->get<std::string>();
      if (!seen.insert(label).second) return wire_error(where, "option '" + label + "' is used twice");
      q.options.emplace_back(label, description);
    }
    if (auto problem = common_problem(instructions, min_confidence)) return wire_error(path, *problem);
    std::vector<Json> labels;
    for (const auto& o : q.options) labels.emplace_back(o.name);
    if (auto problem = labels_problem(labels, "options")) return wire_error(path, *problem);
    return NamedQuestion{name->get<std::string>(), q};
  }

  const Json* levels = member(spec, "levels");
  if (levels == nullptr || !levels->is_array()) return wire_error(path + ".levels", "must be an array of strings");
  if (auto problem = common_problem(instructions, min_confidence)) return wire_error(path, *problem);
  std::vector<Json> labels(levels->begin(), levels->end());
  if (auto problem = labels_problem(labels, "levels")) return wire_error(path, *problem);
  Score q{text, {}, confidence};
  for (const auto& level : labels) q.levels.push_back(level.get<std::string>());
  return NamedQuestion{name->get<std::string>(), q};
}

Json number(double x) { return significant(x); }

}  // namespace

double significant(double x) {
  if (!std::isfinite(x)) return x;
  char buffer[64];
  const auto end = std::to_chars(buffer, buffer + sizeof buffer, x, std::chars_format::general, significant_digits).ptr;
  const Json parsed = Json::parse(std::string(buffer, end), nullptr, /*allow_exceptions=*/false);
  return parsed.is_number() ? parsed.get<double>() : x;
}

Result<Request> decode(const Json& request) {
  static const std::set<std::string> request_fields{"schema_version", "state", "questions", "request_id"};
  if (!request.is_object()) return wire_error("", "the request must be a JSON object");
  if (auto unknown = no_unknown(request, request_fields, "")) return *unknown;
  if (const Json* version = member(request, "schema_version"); version != nullptr && *version != "1") {
    return wire_error("schema_version", "only \"1\" is supported");
  }
  const Json* state = member(request, "state");
  if (state == nullptr) return wire_error("state", "required");
  if (!state->is_string() && !state->is_object() && !state->is_array()) {
    return wire_error("state", "must be text, a JSON object, or an array");
  }
  Request out;
  out.state = *state;
  if (const Json* id = member(request, "request_id"); id != nullptr && !id->is_null()) {
    if (!id->is_string()) return wire_error("request_id", "must be a string");
    out.request_id = id->get<std::string>();
  }
  const Json* specs = member(request, "questions");
  if (specs == nullptr || !specs->is_array() || specs->empty()) return wire_error("questions", "must be a non-empty array");
  std::set<std::string> names;
  for (std::size_t i = 0; i < specs->size(); ++i) {
    const std::string path = "questions[" + std::to_string(i) + "]";
    auto question = decode_question((*specs)[i], path);
    if (!question) return question.error();
    if (!names.insert(question->name).second) {
      return wire_error(path + ".name", "'" + question->name + "' is used twice");
    }
    out.questions.push_back(std::move(question).value());
  }
  return out;
}

Json encode(const Answers& answers, const Questions& questions, const std::optional<std::string>& request_id) {
  Json response = Json::object();
  if (request_id) response["request_id"] = *request_id;
  Json body = Json::object();
  std::shared_ptr<const Call> call;
  for (const auto& [name, answer] : answers) {
    if (!call) call = answer.call;
    const char* type = "boolean";
    for (const auto& nq : questions) {
      if (nq.name != name) continue;
      type = std::holds_alternative<Boolean>(nq.question) ? "boolean"
             : std::holds_alternative<Choice>(nq.question) ? "choice"
                                                           : "score";
    }
    Json value = nullptr;
    if (const auto* b = std::get_if<bool>(&answer.value)) value = *b;
    if (const auto* s = std::get_if<std::string>(&answer.value)) value = *s;
    if (const auto* d = std::get_if<double>(&answer.value)) value = number(*d);
    Json probabilities = Json::object();
    for (const auto& [key, p] : answer.probabilities) probabilities[key] = number(p);
    body[name] = {{"type", type},
                  {"value", value},
                  {"decided", answer.decided()},
                  {"confidence", number(answer.confidence)},
                  {"probabilities", probabilities}};
  }
  response["answers"] = std::move(body);
  if (call) {
    response["call"] = {{"provider", call->provider},
                        {"model", call->model ? Json(*call->model) : Json(nullptr)},
                        {"input_tokens", call->input_tokens ? Json(*call->input_tokens) : Json(nullptr)},
                        {"output_tokens", call->output_tokens ? Json(*call->output_tokens) : Json(nullptr)}};
  } else {
    response["call"] = nullptr;
  }
  return response;
}

Json error(std::string_view code, std::string_view message, const std::optional<std::string>& request_id) {
  Json response = Json::object();
  if (request_id) response["request_id"] = *request_id;
  response["error"] = {{"code", std::string(code)}, {"message", std::string(message)}};
  return response;
}

std::string_view code(const Error& error) noexcept {
  switch (error.kind) {
    case ErrorKind::timeout: return "timeout";
    case ErrorKind::provider_error: return "provider_error";
    default: return "invalid_request";
  }
}

}  // namespace semop::wire
