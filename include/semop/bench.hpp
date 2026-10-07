#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "semop/flows.hpp"
#include "semop/provider.hpp"

namespace semop::bench {

// Score providers against labeled cases: accuracy, how often they said "don't know", the
// probability they gave the right answer, latency, and how stable decisions are when only the
// wording changes. An undecided answer counts as not correct, but not as a miss.

/// An answer reduced to one label: a bool (Boolean), an option (Choice), a level index (Score),
/// or none when undecided. Constructors are explicit about types, so "billing" can't become a
/// bool.
class Decision {
 public:
  Decision() = default;  ///< none: undecided
  Decision(bool value) : v_(value) {}
  Decision(const char* option) : v_(std::string(option)) {}
  Decision(std::string option) : v_(std::move(option)) {}
  Decision(int level) : v_(static_cast<std::int64_t>(level)) {}
  Decision(std::int64_t level) : v_(level) {}

  [[nodiscard]] bool none() const noexcept { return v_.index() == 0; }
  [[nodiscard]] const bool* boolean() const noexcept { return std::get_if<bool>(&v_); }
  [[nodiscard]] const std::string* option() const noexcept { return std::get_if<std::string>(&v_); }
  [[nodiscard]] const std::int64_t* level() const noexcept { return std::get_if<std::int64_t>(&v_); }
  friend bool operator==(const Decision& a, const Decision& b) { return a.v_ == b.v_; }
  friend bool operator!=(const Decision& a, const Decision& b) { return !(a == b); }

 private:
  std::variant<std::monostate, bool, std::string, std::int64_t> v_;
};

/// Reduce an answer to a decision (Score: round half up to a level index).
[[nodiscard]] Decision decide(const Question& question, const Answer& answer);

/// One labeled input. Score labels are level indexes, so they survive rewording the rubric.
struct Case {
  Json state;
  std::vector<std::pair<std::string, Decision>> expected;
};

struct QuestionStats {
  std::size_t correct = 0;
  std::size_t undecided = 0;
  std::size_t total = 0;
  std::vector<double> p_correct;

  [[nodiscard]] std::size_t answered() const noexcept { return total - undecided; }
  [[nodiscard]] double accuracy() const noexcept { return total ? double(correct) / double(total) : 0.0; }
  [[nodiscard]] double accuracy_when_answered() const noexcept {
    return answered() ? double(correct) / double(answered()) : 0.0;
  }
  [[nodiscard]] double mean_p_correct() const noexcept;
};

struct Miss {
  std::size_t case_index = 0;
  std::string question;
  Decision expected;
  Answer got;
};

struct Report {
  std::vector<std::pair<std::string, QuestionStats>> questions;
  std::vector<double> latencies_ms;  ///< per call
  double total_ms = 0;               ///< wall-clock for all cases, excluding warm-up
  std::vector<Miss> misses;          ///< wrong answers (undecided ones aren't misses)
  std::vector<Answers> answers;      ///< per case
  std::vector<std::vector<std::pair<std::string, Decision>>> decisions;  ///< per case

  [[nodiscard]] const QuestionStats* stats(const std::string& name) const;
};

struct RunOptions {
  /// The first `warmup` cases also run once beforehand, untimed.
  std::size_t warmup = 1;
  /// Calls in flight at once. Latency is measured per call from when it starts.
  std::size_t concurrency = 1;
};

/// Ask every case's state all `questions`, one call per case. The first provider error ends the
/// run with that error.
[[nodiscard]] Result<Report> run(Provider& provider, const Questions& questions, const std::vector<Case>& cases,
                                 const RunOptions& options = {});

/// Score already-collected answers (one Answers per case) against the labels.
[[nodiscard]] Result<Report> score(const Questions& questions, const std::vector<Case>& cases,
                                   const std::vector<Answers>& answers, std::vector<double> latencies_ms,
                                   double total_ms);

/// Re-score as if every question had `min_confidence`, without asking again. Only stricter.
[[nodiscard]] Result<Report> at_min_confidence(const Report& report, const Questions& questions,
                                               const std::vector<Case>& cases, double min_confidence);

/// Per question: the fraction of cases whose decision is identical in every report. Pass
/// reports from differently worded versions of the same questions.
[[nodiscard]] std::vector<std::pair<std::string, double>> stability(const std::vector<Report>& reports);

/// How close `order` (ids, best first) is to the ideal order, from 0 to 1, over the first `k`.
/// Gain `2^grade - 1`, discount `log2(position + 2)`. Missing ids count as grade 0. Nullopt when
/// no document is relevant: undefined, not perfect.
[[nodiscard]] std::optional<double> ndcg(const std::vector<std::string>& order,
                                         const std::vector<std::pair<std::string, int>>& grades,
                                         std::size_t k = 10);

/// A whole flow against labeled outcomes.
template <class T>
struct FlowReport {
  std::vector<FlowResult<T>> results;
  std::vector<T> expected;
  std::vector<double> latencies_ms;  ///< per case, all of its calls together

  [[nodiscard]] std::size_t total() const noexcept { return results.size(); }
  /// Cases that stopped at a "don't know".
  [[nodiscard]] std::size_t stopped() const noexcept {
    std::size_t n = 0;
    for (const auto& r : results) n += r.stopped_at.has_value();
    return n;
  }
  /// Cases that failed with an error.
  [[nodiscard]] std::size_t failed() const noexcept {
    std::size_t n = 0;
    for (const auto& r : results) n += r.error.has_value();
    return n;
  }
  [[nodiscard]] std::size_t correct() const noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < results.size(); ++i) n += results[i].decided() && *results[i].value == expected[i];
    return n;
  }
  [[nodiscard]] double accuracy_when_answered() const noexcept {
    const std::size_t answered = total() - stopped() - failed();
    return answered ? double(correct()) / double(answered) : 0.0;
  }
  [[nodiscard]] std::size_t calls() const noexcept {
    std::size_t n = 0;
    for (const auto& r : results) n += r.calls();
    return n;
  }
};

/// Run `fn(ask, input)` for each (input, expected) case and compare. Blocking.
template <class Fn, class Input, class T>
FlowReport<T> run_flow(Fn&& fn, Provider& provider, const std::vector<std::pair<Input, T>>& cases) {
  FlowReport<T> report;
  for (const auto& [input, expected] : cases) {
    const auto start = std::chrono::steady_clock::now();
    report.results.push_back(semop::run_flow(provider, fn, input));
    report.latencies_ms.push_back(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    report.expected.push_back(expected);
  }
  return report;
}

}  // namespace semop::bench
