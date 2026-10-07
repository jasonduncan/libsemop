// Questions, make_answer, and typed operators. Offline; ports tests/test_answers.py and
// tests/test_validation.py from the Python library.
#include <array>
#include <cmath>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <semop/semop.hpp>

using namespace semop;
using Catch::Matchers::ContainsSubstring;

namespace {

const Boolean yes_no{"Is it?"};
const Choice pick{"Which?", {"a", "b", "c"}};
const Score rate{"How much?", {"low", "medium", "high"}};

Answer made(const Question& q, AnswerValue v, std::vector<std::pair<std::string, double>> ps) {
  auto a = make_answer(q, v, std::move(ps));
  REQUIRE(a);
  return std::move(a).value();
}

std::string problem(const Question& q, AnswerValue v, std::vector<std::pair<std::string, double>> ps) {
  auto a = make_answer(q, v, std::move(ps));
  REQUIRE_FALSE(a);
  CHECK(a.error().kind == ErrorKind::provider_error);
  return a.error().message;
}

enum class Team { billing, technical };

}  // namespace

template <>
struct semop::choice_labels<Team> {
  static constexpr std::array values{std::pair{Team::billing, std::string_view{"billing"}},
                                     std::pair{Team::technical, std::string_view{"technical"}}};
};

TEST_CASE("Boolean confidence and ties") {
  const Answer a = made(yes_no, true, {{"true", 0.8}, {"false", 0.2}});
  CHECK(a.boolean() == true);
  CHECK(a.confidence == 0.8);
  CHECK(a.decided());
  const Answer tie = made(yes_no, false, {{"true", 0.5}, {"false", 0.5}});
  CHECK_FALSE(tie.decided());
}

TEST_CASE("a Choice tie is undecided but keeps its probabilities") {
  const Answer a = made(pick, std::string("a"), {{"a", 0.4}, {"b", 0.4}, {"c", 0.2}});
  CHECK_FALSE(a.decided());
  CHECK(a.probabilities == std::vector<std::pair<std::string, double>>{{"a", 0.4}, {"b", 0.4}, {"c", 0.2}});
}

TEST_CASE("probabilities come back in question order") {
  const Answer a = made(pick, std::string("c"), {{"c", 0.7}, {"a", 0.2}, {"b", 0.1}});
  CHECK(a.probabilities[0].first == "a");
  CHECK(a.probabilities[2].first == "c");
  CHECK(a.probability("c") == 0.7);
  CHECK_FALSE(a.probability("zzz"));
}

TEST_CASE("Score confidence is the nearest level") {
  const Answer a = made(rate, 1.6, {{"low", 0.1}, {"medium", 0.2}, {"high", 0.7}});
  CHECK(a.score() == 1.6);
  CHECK(a.confidence == 0.7);  // 1.6 rounds to level 2
}

TEST_CASE("min_confidence withholds low-confidence answers") {
  const Choice strict = Choice{"Which?", {"a", "b"}}.with_min_confidence(0.9);
  CHECK_FALSE(made(strict, std::string("a"), {{"a", 0.85}, {"b", 0.15}}).decided());
  CHECK(made(strict, std::string("a"), {{"a", 0.95}, {"b", 0.05}}).choice() == "a");
}

TEST_CASE("malformed questions are rejected") {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  CHECK(check(Question(Boolean{""}))->message == "instructions must be a non-empty string");
  CHECK(check(Question(Boolean{"   "}))->message == "instructions must be a non-empty string");
  CHECK(check(Question(Boolean{"Is it?"}.with_min_confidence(1.5)))->message ==
        "min_confidence must be a number from 0 to 1, got 1.5");
  CHECK(check(Question(Boolean{"Is it?"}.with_min_confidence(nan)))->message ==
        "min_confidence must be a number from 0 to 1, got nan");
  CHECK(check(Question(Choice{"Which?", {"a"}}))->message == "need at least 2 options, got 1");
  CHECK(check(Question(Choice{"Which?", {"", "b"}}))->message == "options must be non-empty strings");
  CHECK(check(Question(Choice{"Which?", {"a", "a"}}))->message == "options must be unique");
  CHECK(check(Question(Score{"How much?", {"x", "x"}}))->message == "levels must be unique");
  CHECK(check(Question(Score{"How much?", {"only one"}}))->message == "need at least 2 levels, got 1");
  CHECK_FALSE(check(Question(yes_no)));
  CHECK(check(Question(Boolean{""}))->kind == ErrorKind::invalid_question);
}

TEST_CASE("question sets need names, unique names, and valid questions") {
  CHECK(check(Questions{})->message == "at least one question is required");
  CHECK(check(Questions{{"", yes_no}})->message == "question names must be non-empty");
  CHECK(check(Questions{{"b", yes_no}, {"a", yes_no}, {"b", pick}, {"a", rate}})->message ==
        "operator names must be unique; repeated: a, b");
  const auto bad = check(Questions{{"ok", yes_no}, {"rubric", Score{"How much?", {"one"}}}});
  REQUIRE(bad);
  CHECK(bad->message == "rubric: need at least 2 levels, got 1");
  CHECK(bad->path == "rubric");
}

TEST_CASE("malformed provider output never becomes an answer") {
  const Boolean strict_yes = yes_no.with_min_confidence(0.9);
  const Choice ab{"Which?", {"a", "b"}};
  const double nan = std::numeric_limits<double>::quiet_NaN();
  CHECK(problem(strict_yes, true, {{"true", nan}, {"false", nan}}) == "probabilities must be finite numbers from 0 to 1");
  CHECK(problem(strict_yes, true, {{"true", 1.2}, {"false", -0.2}}) == "probabilities must be finite numbers from 0 to 1");
  CHECK(problem(strict_yes, true, {{"true", 0.2}, {"false", 0.8}}) == "Boolean value True disagrees with its probabilities");
  CHECK(problem(strict_yes, std::string("yes"), {{"true", 0.9}, {"false", 0.1}}) ==
        "Boolean value 'yes' disagrees with its probabilities");
  CHECK(problem(ab, std::string("zzz"), {{"a", 0.5}, {"b", 0.5}}) == "Choice value 'zzz' is not one of the options");
  CHECK(problem(ab, std::string("b"), {{"a", 0.95}, {"b", 0.05}}) == "Choice value 'b' is not the most likely option");
  CHECK(problem(ab, std::string("a"), {{"a", 0.9}}) == "probabilities must cover exactly ['a', 'b']");
  CHECK(problem(ab, std::string("a"), {{"a", 0.9}, {"b", 0.9}}) == "probabilities must sum to 1");
  CHECK(problem(ab, std::string("a"), {{"a", 0.9}, {"a", 0.1}}) == "probabilities must cover exactly ['a', 'b']");
  CHECK(problem(ab, std::string("a"), {{"a", 0.5}, {"b", 0.4}, {"c", 0.1}}) ==
        "probabilities must cover exactly ['a', 'b']");
  CHECK(problem(rate, 5.0, {{"low", 0.1}, {"medium", 0.2}, {"high", 0.7}}) == "Score value 5.0 is outside 0..2");
  CHECK(problem(rate, 0.3, {{"low", 0.1}, {"medium", 0.2}, {"high", 0.7}}) ==
        "Score value 0.3 disagrees with its probabilities");
}

TEST_CASE("rounded probabilities are accepted") {
  // Laya rounds to 4 decimals, so totals can be slightly off 1.
  const Answer laya = made(Choice{"Which?", {"a", "b"}}, std::string("a"), {{"a", 0.6667}, {"b", 0.3334}});
  CHECK(laya.choice() == "a");
  CHECK(laya.confidence == 0.6667);
  // A real TypeSafe answer: 2-decimal rounding puts the score 0.01 off its expectation.
  const Answer ts = made(rate, 0.58, {{"low", 0.43}, {"medium", 0.57}, {"high", 0.0}});
  CHECK(ts.score() == 0.58);
  CHECK(ts.confidence == 0.57);
}

TEST_CASE("complement has no float noise") {
  CHECK(1 - 0.07 != 0.93);
  CHECK(complement(0.07) == 0.93);
  const Boolean strict = yes_no.with_min_confidence(0.93);
  const Answer a = made(strict, false, {{"true", 0.07}, {"false", complement(0.07)}});
  CHECK(a.boolean() == false);  // would be undecided with 1 - 0.07
}

TEST_CASE("typed operators read typed values") {
  const Operator urgent{"urgent", Boolean{"Is it urgent?"}};
  const Operator team{"team", EnumChoice<Team>{"Which team?", {{Team::technical, "Bugs"}}}};
  const Operator topic{"topic", Choice{"What topic?", {"a", "b"}}};
  const Operator level{"level", Score{"How much?", {"low", "high"}}};

  const NamedQuestion named = team;
  const auto& choice = std::get<Choice>(named.question);
  REQUIRE(choice.options.size() == 2);
  CHECK(choice.options[0].name == "billing");
  CHECK_FALSE(choice.options[0].description);
  CHECK(choice.options[1].description == "Bugs");

  Answers answers;
  answers.add("urgent", made(urgent.question, true, {{"true", 0.9}, {"false", 0.1}}));
  answers.add("team", made(choice, std::string("technical"), {{"billing", 0.2}, {"technical", 0.8}}));
  answers.add("topic", made(topic.question, std::string("b"), {{"a", 0.5}, {"b", 0.5}}));  // tie
  answers.add("level", made(level.question, 0.9, {{"low", 0.1}, {"high", 0.9}}));

  std::optional<bool> u = answers.value(urgent);
  std::optional<Team> t = answers.value(team);
  std::optional<std::string> tp = answers.value(topic);
  std::optional<double> l = answers.value(level);
  CHECK(u == true);
  CHECK(t == Team::technical);
  CHECK_FALSE(tp);  // undecided
  CHECK(l == 0.9);
  CHECK(answers[urgent].confidence == 0.9);
}
