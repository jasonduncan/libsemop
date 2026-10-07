#include "semop/answers.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "util.hpp"

namespace semop {

namespace {

bool is_probability(double p) { return std::isfinite(p) && p >= 0 && p <= 1; }

std::string_view instructions_of(const Question& question) {
  return std::visit([](const auto& q) -> std::string_view { return q.instructions; }, question);
}

double min_confidence_of(const Question& question) {
  return std::visit([](const auto& q) { return q.min_confidence; }, question);
}

}  // namespace

// ---------------------------------------------------------------------------
// Questions
// ---------------------------------------------------------------------------

namespace {

std::optional<std::string> check_labels(const std::vector<std::string>& labels, const char* what) {
  if (labels.size() < 2) return "need at least 2 " + std::string(what) + ", got " + std::to_string(labels.size());
  for (const auto& label : labels) {
    if (label.empty()) return std::string(what) + " must be non-empty strings";
  }
  if (std::set<std::string>(labels.begin(), labels.end()).size() != labels.size()) {
    return std::string(what) + " must be unique";
  }
  return std::nullopt;
}

std::optional<std::string> problem_with(const Question& question) {
  if (detail::blank(instructions_of(question))) return "instructions must be a non-empty string";
  const double c = min_confidence_of(question);
  if (!std::isfinite(c) || c < 0 || c > 1) {
    return "min_confidence must be a number from 0 to 1, got " + detail::repr(c);
  }
  if (const auto* choice = std::get_if<Choice>(&question)) {
    std::vector<std::string> names;
    for (const auto& option : choice->options) names.push_back(option.name);
    return check_labels(names, "options");
  }
  if (const auto* score = std::get_if<Score>(&question)) return check_labels(score->levels, "levels");
  return std::nullopt;
}

}  // namespace

std::optional<Error> check(const Question& question) {
  if (auto problem = problem_with(question)) return detail::invalid(ErrorKind::invalid_question, std::move(*problem));
  return std::nullopt;
}

std::optional<Error> check(const Questions& questions) {
  if (questions.empty()) return detail::invalid(ErrorKind::invalid_question, "at least one question is required");
  std::set<std::string> seen;
  std::set<std::string> repeated;
  for (const auto& [name, question] : questions) {
    if (name.empty()) return detail::invalid(ErrorKind::invalid_question, "question names must be non-empty");
    if (!seen.insert(name).second) repeated.insert(name);
    if (auto problem = problem_with(question)) {
      return detail::invalid(ErrorKind::invalid_question, name + ": " + *problem, name);
    }
  }
  if (!repeated.empty()) {
    std::string list;
    for (const auto& name : repeated) list += (list.empty() ? "" : ", ") + name;
    return detail::invalid(ErrorKind::invalid_question, "operator names must be unique; repeated: " + list);
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Answers
// ---------------------------------------------------------------------------

std::optional<bool> Answer::boolean() const {
  if (const auto* b = std::get_if<bool>(&value)) return *b;
  return std::nullopt;
}

std::optional<std::string> Answer::choice() const {
  if (const auto* s = std::get_if<std::string>(&value)) return *s;
  return std::nullopt;
}

std::optional<double> Answer::score() const {
  if (const auto* d = std::get_if<double>(&value)) return *d;
  return std::nullopt;
}

std::optional<double> Answer::probability(std::string_view key) const {
  for (const auto& [k, p] : probabilities) {
    if (k == key) return p;
  }
  return std::nullopt;
}

double complement(double p) noexcept { return std::round((1 - p) * 1e10) / 1e10; }

Result<Answer> make_answer(const Question& question, const AnswerValue& value,
                           std::vector<std::pair<std::string, double>> probabilities, Json raw,
                           std::shared_ptr<const Call> call) {
  const auto fail = [](std::string message) { return detail::invalid(ErrorKind::provider_error, std::move(message)); };

  std::vector<std::string> outcomes;
  if (std::holds_alternative<Boolean>(question)) {
    outcomes = {"true", "false"};
  } else if (const auto* choice = std::get_if<Choice>(&question)) {
    for (const auto& option : choice->options) outcomes.push_back(option.name);
  } else {
    outcomes = std::get<Score>(question).levels;
  }

  // The distribution: exactly the question's outcomes, finite, in [0, 1], summing to 1.
  std::vector<std::pair<std::string, double>> ordered;
  {
    std::set<std::string> keys;
    for (const auto& [k, p] : probabilities) keys.insert(k);
    const std::set<std::string> expected(outcomes.begin(), outcomes.end());
    if (keys != expected || keys.size() != probabilities.size()) {
      return fail("probabilities must cover exactly " + detail::repr(outcomes));
    }
    for (const auto& outcome : outcomes) {
      for (const auto& [k, p] : probabilities) {
        if (k == outcome) ordered.emplace_back(k, p);
      }
    }
  }
  std::vector<double> ps;
  for (const auto& [k, p] : ordered) {
    if (!is_probability(p)) return fail("probabilities must be finite numbers from 0 to 1");
    ps.push_back(p);
  }
  if (std::fabs(detail::fsum(ps) - 1) > rounding * double(outcomes.size()) + 1e-9) {
    return fail("probabilities must sum to 1");
  }

  bool tied = false;
  double confidence = 0;
  if (std::holds_alternative<Boolean>(question)) {
    const double p_true = ps[0];
    const double p_false = ps[1];
    tied = p_true == p_false;
    const auto* b = std::get_if<bool>(&value);
    if (b == nullptr || (!tied && *b != (p_true > p_false))) {
      return fail("Boolean value " + detail::repr(value) + " disagrees with its probabilities");
    }
    confidence = std::max(p_true, p_false);
  } else if (std::holds_alternative<Choice>(question)) {
    const auto* chosen = std::get_if<std::string>(&value);
    const auto at = chosen ? std::find(outcomes.begin(), outcomes.end(), *chosen) : outcomes.end();
    if (at == outcomes.end()) return fail("Choice value " + detail::repr(value) + " is not one of the options");
    const double top = *std::max_element(ps.begin(), ps.end());
    tied = std::count(ps.begin(), ps.end(), top) > 1;
    const double p = ps[std::size_t(at - outcomes.begin())];
    if (p != top) return fail("Choice value " + detail::repr(value) + " is not the most likely option");
    confidence = p;
  } else {
    const std::size_t highest = outcomes.size() - 1;
    const auto* s = std::get_if<double>(&value);
    if (s == nullptr || !std::isfinite(*s) || *s < 0 || *s > double(highest)) {
      return fail("Score value " + detail::repr(value) + " is outside 0.." + std::to_string(highest));
    }
    double expected = 0;
    for (std::size_t i = 0; i < ps.size(); ++i) expected += double(i) * ps[i];
    const double n = double(outcomes.size());
    const double margin = rounding * (1 + n * (n - 1) / 2);  // the score, plus each level
    if (std::fabs(*s - expected) > margin) {
      return fail("Score value " + detail::repr(value) + " disagrees with its probabilities");
    }
    confidence = ps[std::min(std::size_t(*s + 0.5), highest)];
  }

  Answer answer;
  answer.value = (tied || confidence < min_confidence_of(question)) ? AnswerValue() : value;
  answer.probabilities = std::move(ordered);
  answer.confidence = confidence;
  answer.call = std::move(call);
  answer.raw = std::move(raw);
  return answer;
}

std::string_view to_string(ErrorKind kind) noexcept {
  switch (kind) {
    case ErrorKind::invalid_question: return "invalid_question";
    case ErrorKind::invalid_argument: return "invalid_argument";
    case ErrorKind::invalid_request: return "invalid_request";
    case ErrorKind::provider_error: return "provider_error";
    case ErrorKind::timeout: return "timeout";
  }
  return "unknown";
}

}  // namespace semop
