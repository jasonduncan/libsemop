// Benchmarking, offline. Ports the scoring cases from tests/test_answers.py and the ndcg test.
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "scripted.hpp"

using namespace semop;

namespace {

const Choice pick{"Which?", {"a", "b", "c"}};

Answers one(const std::string& name, const Question& q, AnswerValue v, std::vector<std::pair<std::string, double>> ps) {
  Answers answers;
  answers.add(name, make_answer(q, v, std::move(ps)).value());
  return answers;
}

}  // namespace

TEST_CASE("scoring counts undecided separately from misses") {
  const Questions questions{{"q", pick}};
  const std::vector<bench::Case> cases = {{"1", {{"q", "a"}}}, {"2", {{"q", "b"}}}, {"3", {{"q", "c"}}}};
  const std::vector<Answers> answers = {
      one("q", pick, std::string("a"), {{"a", 0.9}, {"b", 0.05}, {"c", 0.05}}),  // right, sure
      one("q", pick, std::string("a"), {{"a", 0.6}, {"b", 0.3}, {"c", 0.1}}),    // wrong, unsure
      one("q", pick, std::string("a"), {{"a", 0.4}, {"b", 0.4}, {"c", 0.2}}),    // tie: undecided
  };
  auto report = bench::score(questions, cases, answers, {1, 1, 1}, 3);
  REQUIRE(report);
  const auto* s = report->stats("q");
  CHECK(s->correct == 1);
  CHECK(s->undecided == 1);
  CHECK(s->answered() == 2);
  CHECK(report->misses.size() == 1);
  CHECK(s->mean_p_correct() == Catch::Approx((0.9 + 0.3 + 0.2) / 3));

  auto strict = bench::at_min_confidence(*report, questions, cases, 0.8);
  REQUIRE(strict);
  CHECK(strict->stats("q")->correct == 1);
  CHECK(strict->stats("q")->undecided == 2);
  CHECK(strict->stats("q")->accuracy_when_answered() == 1.0);
}

TEST_CASE("labels are typed: an option can't pass for a bool") {
  bench::Decision option = "billing";
  CHECK(option.option() != nullptr);
  CHECK(option.boolean() == nullptr);
  bench::Decision level = 2;
  CHECK(*level.level() == 2);
  CHECK(bench::Decision(true) != bench::Decision(1));
}

TEST_CASE("run asks each case once (plus warm-up) and scores; concurrency keeps case order") {
  const Operator yes{"yes", Boolean{"Is it yes?"}};
  scripted::Provider provider("Fake", [&](const Json& state, const Questions& qs) {
    const bool is_yes = state.get<std::string>() == "yes";
    Answers answers;
    answers.add(qs[0].name, scripted::answer(qs[0].question, is_yes, 0.9));
    return Result<Answers>(answers);
  });
  const std::vector<bench::Case> cases = {{"yes", {{"yes", true}}}, {"no", {{"yes", false}}}, {"yes", {{"yes", false}}}};
  auto report = bench::run(provider, questions({yes}), cases, {1, 3});
  REQUIRE(report);
  CHECK(provider.calls.size() == 4);  // one warm-up + three cases
  CHECK(provider.peak <= 3);
  CHECK(report->stats("yes")->correct == 2);
  CHECK(report->misses.size() == 1);
  CHECK(report->misses[0].case_index == 2);
  CHECK(report->latencies_ms.size() == 3);
  CHECK(*report->decisions[1][0].second.boolean() == false);
}

TEST_CASE("run reports the first provider error") {
  FunctionProvider down("Down", [](const Json&, const Questions&) -> Result<Answers> {
    Error e;
    e.kind = ErrorKind::provider_error;
    e.message = "outage";
    return e;
  });
  auto report = bench::run(down, Questions{{"x", Boolean{"Is it?"}}}, {{"a", {}}});
  REQUIRE_FALSE(report);
  CHECK(report.error().message == "Down: outage");
}

TEST_CASE("stability compares decisions across wordings") {
  const Questions questions{{"q", pick}};
  const std::vector<bench::Case> cases = {{"1", {}}, {"2", {}}};
  auto a = bench::score(questions, cases,
                        {one("q", pick, std::string("a"), {{"a", 0.9}, {"b", 0.05}, {"c", 0.05}}),
                         one("q", pick, std::string("b"), {{"a", 0.05}, {"b", 0.9}, {"c", 0.05}})},
                        {}, 0);
  auto b = bench::score(questions, cases,
                        {one("q", pick, std::string("a"), {{"a", 0.9}, {"b", 0.05}, {"c", 0.05}}),
                         one("q", pick, std::string("c"), {{"a", 0.05}, {"b", 0.05}, {"c", 0.9}})},
                        {}, 0);
  auto s = bench::stability({*a, *b});
  REQUIRE(s.size() == 1);
  CHECK(s[0].second == 0.5);
}

TEST_CASE("ndcg") {
  const std::vector<std::pair<std::string, int>> grades = {{"A", 2}, {"B", 0}, {"C", 1}};
  CHECK(bench::ndcg({"A", "C", "B"}, grades) == 1.0);
  const double ideal = 3 / std::log2(2.0) + 1 / std::log2(3.0);
  CHECK(*bench::ndcg({"B", "C", "A"}, grades) == Catch::Approx((1 / std::log2(3.0) + 3 / std::log2(4.0)) / ideal));
  CHECK_FALSE(bench::ndcg({"A", "B"}, {{"A", 0}, {"B", 0}}));
}
