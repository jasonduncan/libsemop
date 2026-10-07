#include "semop/bench.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <set>

#include "util.hpp"

namespace semop::bench {

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// The Answer::probabilities key for a label: "true"/"false", an option, or a level's text.
std::optional<std::string> key_for(const Question& question, const Decision& label) {
  if (std::holds_alternative<Boolean>(question)) {
    if (const bool* b = label.boolean()) return std::string(*b ? "true" : "false");
  } else if (std::holds_alternative<Choice>(question)) {
    if (const std::string* option = label.option()) return *option;
  } else {
    const auto& levels = std::get<Score>(question).levels;
    if (const std::int64_t* level = label.level()) {
      if (*level >= 0 && std::size_t(*level) < levels.size()) return levels[std::size_t(*level)];
    }
  }
  return std::nullopt;
}

}  // namespace

Decision decide(const Question& question, const Answer& answer) {
  if (!answer.decided()) return {};
  if (std::holds_alternative<Boolean>(question)) return *answer.boolean();
  if (std::holds_alternative<Choice>(question)) return *answer.choice();
  return std::int64_t(*answer.score() + 0.5);
}

double QuestionStats::mean_p_correct() const noexcept {
  if (p_correct.empty()) return 0.0;
  return detail::fsum(p_correct) / double(p_correct.size());
}

const QuestionStats* Report::stats(const std::string& name) const {
  for (const auto& [n, s] : questions) {
    if (n == name) return &s;
  }
  return nullptr;
}

Result<Report> score(const Questions& questions, const std::vector<Case>& cases, const std::vector<Answers>& answers,
                     std::vector<double> latencies_ms, double total_ms) {
  if (answers.size() != cases.size()) {
    return detail::invalid(ErrorKind::invalid_argument, "need one set of answers per case");
  }
  Report report;
  for (const auto& nq : questions) report.questions.emplace_back(nq.name, QuestionStats{});
  const auto question_named = [&](const std::string& name) -> const NamedQuestion* {
    for (const auto& nq : questions) {
      if (nq.name == name) return &nq;
    }
    return nullptr;
  };

  for (std::size_t i = 0; i < cases.size(); ++i) {
    std::vector<std::pair<std::string, Decision>> decisions;
    for (const auto& nq : questions) {
      const Answer* answer = answers[i].find(nq.name);
      if (answer == nullptr) {
        return detail::invalid(ErrorKind::invalid_argument,
                               "case " + std::to_string(i) + " has no answer for '" + nq.name + "'");
      }
      decisions.emplace_back(nq.name, decide(nq.question, *answer));
    }
    for (const auto& [name, label] : cases[i].expected) {
      const NamedQuestion* nq = question_named(name);
      const auto key = nq ? key_for(nq->question, label) : std::nullopt;
      if (!key) {
        return detail::invalid(ErrorKind::invalid_argument,
                               "case " + std::to_string(i) + ": label for '" + name + "' doesn't fit the question");
      }
      QuestionStats* stats = nullptr;
      for (auto& [n, st] : report.questions) {
        if (n == name) stats = &st;
      }
      QuestionStats& s = *stats;
      const Answer& answer = answers[i].at(name);
      Decision decision;
      for (const auto& [n, d] : decisions) {
        if (n == name) decision = d;
      }
      s.total += 1;
      s.p_correct.push_back(answer.probability(*key).value_or(0));
      if (decision.none()) {
        s.undecided += 1;
      } else if (decision == label) {
        s.correct += 1;
      } else {
        report.misses.push_back(Miss{i, name, label, answer});
      }
    }
    report.decisions.push_back(std::move(decisions));
  }
  report.latencies_ms = std::move(latencies_ms);
  report.total_ms = total_ms;
  report.answers = answers;
  return report;
}

Result<Report> run(Provider& provider, const Questions& questions, const std::vector<Case>& cases,
                   const RunOptions& options) {
  if (options.concurrency < 1) return detail::invalid(ErrorKind::invalid_argument, "concurrency must be a positive integer");
  if (auto problem = check(questions)) return *problem;
  for (std::size_t i = 0; i < std::min(options.warmup, cases.size()); ++i) {
    auto warm = ask(provider, cases[i].state, questions);
    if (!warm) return warm.error();
  }

  std::vector<std::optional<Result<Answers>>> results(cases.size());
  std::vector<double> latencies(cases.size());
  std::vector<Clock::time_point> started(cases.size());
  std::size_t next = 0;
  std::size_t running = 0;
  std::size_t finished = 0;
  const auto begin = Clock::now();
  while (finished < cases.size()) {
    while (next < cases.size() && running < options.concurrency) {
      const std::size_t i = next++;
      ++running;
      started[i] = Clock::now();
      provider.start(cases[i].state, questions, [&, i](Result<Answers> result) {
        latencies[i] = ms_since(started[i]);
        results[i].emplace(std::move(result));
        --running;
        ++finished;
      });
    }
    const std::size_t before = finished;
    provider.pump();
    if (finished == before && finished < cases.size()) provider.wait(std::chrono::milliseconds(10));
  }
  const double total = ms_since(begin);

  std::vector<Answers> answers;
  for (auto& result : results) {
    if (!*result) return result->error();
    answers.push_back(std::move(*result).value());
  }
  return score(questions, cases, answers, std::move(latencies), total);
}

Result<Report> at_min_confidence(const Report& report, const Questions& questions, const std::vector<Case>& cases,
                                 double min_confidence) {
  std::vector<Answers> stricter;
  for (const auto& case_answers : report.answers) {
    Answers strict;
    for (const auto& [name, answer] : case_answers) {
      Answer copy = answer;
      if (copy.confidence < min_confidence) copy.value = AnswerValue();
      strict.add(name, std::move(copy));
    }
    stricter.push_back(std::move(strict));
  }
  return score(questions, cases, stricter, report.latencies_ms, report.total_ms);
}

std::vector<std::pair<std::string, double>> stability(const std::vector<Report>& reports) {
  std::vector<std::pair<std::string, double>> out;
  if (reports.empty() || reports[0].decisions.empty()) return out;
  const std::size_t cases = reports[0].decisions.size();
  for (const auto& [name, first] : reports[0].decisions[0]) {
    std::size_t stable = 0;
    for (std::size_t i = 0; i < cases; ++i) {
      bool same = true;
      Decision reference;
      bool have = false;
      for (const auto& report : reports) {
        for (const auto& [n, d] : report.decisions[i]) {
          if (n != name) continue;
          if (!have) {
            reference = d;
            have = true;
          } else if (d != reference) {
            same = false;
          }
        }
      }
      stable += same;
    }
    out.emplace_back(name, double(stable) / double(cases));
  }
  return out;
}

std::optional<double> ndcg(const std::vector<std::string>& order, const std::vector<std::pair<std::string, int>>& grades,
                           std::size_t k) {
  const auto grade_of = [&](const std::string& id) {
    for (const auto& [g_id, g] : grades) {
      if (g_id == id) return g;
    }
    return 0;
  };
  const auto dcg = [&](const std::vector<std::string>& ids) {
    std::vector<double> gains;
    for (std::size_t i = 0; i < ids.size() && i < k; ++i) {
      gains.push_back((std::pow(2.0, grade_of(ids[i])) - 1) / std::log2(double(i) + 2));
    }
    return detail::fsum(gains);
  };
  std::vector<std::pair<std::string, int>> sorted = grades;
  std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  std::vector<std::string> ideal_ids;
  for (const auto& [id, g] : sorted) ideal_ids.push_back(id);
  const double ideal = dcg(ideal_ids);
  if (ideal == 0) return std::nullopt;
  return dcg(order) / ideal;
}

}  // namespace semop::bench
