#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <semop/semop.hpp>

namespace scripted {

/// An answer in the style of the Python tests' Scripted providers: `value` with probability `p`.
inline semop::Answer answer(const semop::Question& question, const semop::AnswerValue& value, double p,
                            std::shared_ptr<const semop::Call> call = nullptr) {
  std::vector<std::pair<std::string, double>> probabilities;
  if (std::holds_alternative<semop::Boolean>(question)) {
    const bool yes = std::get<bool>(value);
    probabilities = {{"true", yes ? p : semop::complement(p)}, {"false", yes ? semop::complement(p) : p}};
  } else if (const auto* choice = std::get_if<semop::Choice>(&question)) {
    const auto& chosen = std::get<std::string>(value);
    const double rest = choice->options.size() > 1 ? (1 - p) / double(choice->options.size() - 1) : 0;
    for (const auto& o : choice->options) probabilities.emplace_back(o.name, o.name == chosen ? p : rest);
  } else {
    const auto& levels = std::get<semop::Score>(question).levels;
    const auto level = std::size_t(std::lround(std::get<double>(value)));
    for (std::size_t i = 0; i < levels.size(); ++i) probabilities.emplace_back(levels[i], i == level ? 1.0 : 0.0);
    auto built = semop::make_answer(question, semop::AnswerValue(double(level)), probabilities, nullptr, call);
    return std::move(built).value();
  }
  auto built = semop::make_answer(question, value, probabilities, nullptr, call);
  SEMOP_ASSERT(built.ok(), "scripted answer must be valid");
  return std::move(built).value();
}

/// A non-blocking provider. `script(state, questions)` makes each result; results are held
/// until `release` (or delivered on the next pump when `hold` is false). Records every call.
class Provider : public semop::Provider {
 public:
  using Script = std::function<semop::Result<semop::Answers>(const semop::Json&, const semop::Questions&)>;

  Provider(std::string name, Script script) : semop::Provider(std::move(name)), script_(std::move(script)) {}

  struct Asked {
    semop::Ticket ticket;
    semop::Json state;
    std::vector<std::string> names;
    bool cancelled = false;
    bool finished = false;
  };
  std::vector<Asked> calls;
  bool hold = false;
  std::size_t running = 0;
  std::size_t peak = 0;

  /// Deliver held calls, newest first (so ordering bugs show).
  void release() {
    for (auto it = calls.rbegin(); it != calls.rend(); ++it) complete(*it);
  }

 protected:
  void start_call(semop::Ticket ticket, const semop::Json& state, const semop::Questions& questions) override {
    Asked asked{ticket, state, {}};
    for (const auto& q : questions) asked.names.push_back(q.name);
    calls.push_back(asked);
    questions_[ticket] = questions;
    peak = std::max(peak, ++running);
  }
  void cancel_call(semop::Ticket ticket) override {
    for (auto& c : calls) {
      if (c.ticket == ticket && !c.finished) {
        c.cancelled = true;
        c.finished = true;
        --running;
      }
    }
  }
  void on_pump() override {
    if (!hold) {
      for (auto& c : calls) complete(c);
    }
  }

 private:
  void complete(Asked& c) {
    if (c.finished) return;
    c.finished = true;
    --running;
    finish(c.ticket, script_(c.state, questions_[c.ticket]));
  }

  Script script_;
  std::map<semop::Ticket, semop::Questions> questions_;
};

}  // namespace scripted
