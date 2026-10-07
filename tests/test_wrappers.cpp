// Cascade and TimeLimited, offline. Ports tests/test_cascade.py and the timeout tests.
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "scripted.hpp"

using namespace semop;

namespace {

const Boolean sure = Boolean{"Is it?"}.with_min_confidence(0.8);
const Choice pick = Choice{"Which?", {"a", "b"}}.with_min_confidence(0.8);

/// Answers each question with confidence[name] toward true / "a", tagged with its own Call.
scripted::Provider scripted_provider(std::string name, std::map<std::string, double> confidence) {
  auto call = std::make_shared<const Call>(Call{name, std::nullopt, std::nullopt, std::nullopt});
  return scripted::Provider(name, [confidence, call](const Json&, const Questions& questions) {
    Answers answers;
    for (const auto& [n, q] : questions) {
      const double p = confidence.at(n);
      const AnswerValue value = std::holds_alternative<Boolean>(q) ? AnswerValue(true) : AnswerValue(std::string("a"));
      const std::vector<std::pair<std::string, double>> ps =
          std::holds_alternative<Boolean>(q) ? std::vector<std::pair<std::string, double>>{{"true", p}, {"false", 1 - p}}
                                             : std::vector<std::pair<std::string, double>>{{"a", p}, {"b", 1 - p}};
      answers.add(n, make_answer(q, value, ps, nullptr, call).value());
    }
    return Result<Answers>(answers);
  });
}

std::vector<std::vector<std::string>> asked(const scripted::Provider& p) {
  std::vector<std::vector<std::string>> out;
  for (const auto& c : p.calls) out.push_back(c.names);
  return out;
}

}  // namespace

TEST_CASE("cascade: only unsure answers escalate, in one call") {
  auto fast = scripted_provider("fast", {{"x", 0.95}, {"y", 0.6}, {"z", 0.7}});
  auto strong = scripted_provider("strong", {{"y", 0.9}, {"z", 0.99}});
  Cascade cascade({&fast, &strong});
  auto answers = ask(cascade, "hi", Questions{{"x", sure}, {"y", pick}, {"z", sure}});
  REQUIRE(answers);
  CHECK(asked(fast) == std::vector<std::vector<std::string>>{{"x", "y", "z"}});
  CHECK(asked(strong) == std::vector<std::vector<std::string>>{{"y", "z"}});
  CHECK(answers->at("x").call->provider == "fast");
  CHECK(answers->at("y").call->provider == "strong");
  CHECK(answers->at("z").call->provider == "strong");
  std::vector<std::string> order;
  for (const auto& [name, a] : *answers) order.push_back(name);
  CHECK(order == std::vector<std::string>{"x", "y", "z"});
}

TEST_CASE("cascade: nothing escalates when the first is sure") {
  auto fast = scripted_provider("fast", {{"x", 0.9}});
  auto strong = scripted_provider("strong", {});
  Cascade cascade({&fast, &strong});
  REQUIRE(ask(cascade, "hi", Questions{{"x", sure}}));
  CHECK(strong.calls.empty());
}

TEST_CASE("cascade: the last provider's answer stands even if undecided") {
  auto a = scripted_provider("a", {{"x", 0.6}});
  auto b = scripted_provider("b", {{"x", 0.7}});
  auto c = scripted_provider("c", {{"x", 0.55}});
  Cascade cascade({&a, &b, &c});
  auto answers = ask(cascade, "hi", Questions{{"x", sure}});
  REQUIRE(answers);
  CHECK_FALSE(answers->at("x").decided());
  CHECK(answers->at("x").call->provider == "c");
  CHECK(a.calls.size() == 1);
  CHECK(b.calls.size() == 1);
  CHECK(c.calls.size() == 1);
}

TEST_CASE("cascade: errors are the result, not a reason to escalate") {
  FunctionProvider down("Down", [](const Json&, const Questions&) -> Result<Answers> {
    Error e;
    e.kind = ErrorKind::provider_error;
    e.message = "outage";
    return e;
  });
  auto strong = scripted_provider("strong", {{"x", 0.99}});
  Cascade cascade({&down, &strong});
  auto r = ask(cascade, "hi", Questions{{"x", sure}});
  REQUIRE_FALSE(r);
  CHECK(r.error().message == "Down: outage");
  CHECK(strong.calls.empty());
}

TEST_CASE("cascade: escalate_below is separate from the final don't-know") {
  const Boolean always{"Is it?"};
  auto fast = scripted_provider("fast", {{"x", 0.7}, {"y", 0.95}});
  auto strong = scripted_provider("strong", {{"x", 0.6}});
  Cascade cascade({&fast, &strong}, 0.8);
  auto answers = ask(cascade, "hi", Questions{{"x", always}, {"y", always}});
  REQUIRE(answers);
  CHECK(asked(strong) == std::vector<std::vector<std::string>>{{"x"}});
  CHECK(answers->at("x").decided());
  CHECK(answers->at("x").call->provider == "strong");
  CHECK(answers->at("y").call->provider == "fast");
}

TEST_CASE("cascade: works as an operator's provider") {
  auto fast = scripted_provider("fast", {{"x", 0.6}});
  auto strong = scripted_provider("strong", {{"x", 0.9}});
  Cascade cascade({&fast, &strong});
  const Operator op{"x", sure};
  auto a = ask(cascade, "hi", op);
  REQUIRE(a);
  CHECK(a->call->provider == "strong");
}

TEST_CASE("cascade: bad setups fail every call") {
  auto only = scripted_provider("a", {});
  auto other = scripted_provider("b", {});
  Cascade one({&only});
  Cascade high({&only, &other}, 1.5);
  Cascade nan({&only, &other}, std::numeric_limits<double>::quiet_NaN());
  for (Cascade* c : {&one, &high, &nan}) {
    auto r = ask(*c, "hi", Questions{{"x", sure}});
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == ErrorKind::invalid_argument);
  }
  CHECK(only.calls.empty());
}

TEST_CASE("TimeLimited: a call that runs over is cancelled and reported as a timeout") {
  auto slow = scripted_provider("Slow", {{"x", 0.9}});
  slow.hold = true;
  TimeLimited limited(slow, std::chrono::milliseconds(2000));
  auto now = std::chrono::steady_clock::time_point{};
  limited.now = [&] { return now; };

  std::optional<Result<Answers>> out;
  limited.start("hi", Questions{{"x", sure}}, [&](Result<Answers> r) { out.emplace(std::move(r)); });
  limited.pump();
  CHECK_FALSE(out);
  now += std::chrono::milliseconds(2000);
  limited.pump();
  REQUIRE(out);
  REQUIRE_FALSE(*out);
  CHECK(out->error().kind == ErrorKind::timeout);
  CHECK(out->error().is_provider_error());
  CHECK(out->error().message == "Slow: no answer within 2000ms");
  CHECK(slow.calls[0].cancelled);
}

TEST_CASE("TimeLimited: answers in time pass through") {
  auto quick = scripted_provider("Quick", {{"x", 0.9}});
  TimeLimited limited(quick, std::chrono::milliseconds(2000));
  auto r = ask(limited, "hi", Questions{{"x", sure}});
  REQUIRE(r);
  CHECK(r->at("x").call->provider == "Quick");
}

TEST_CASE("wrappers cancel inner calls when destroyed") {
  auto inner = scripted_provider("Inner", {{"x", 0.9}});
  inner.hold = true;
  {
    TimeLimited limited(inner, std::chrono::milliseconds(1000));
    limited.start("hi", Questions{{"x", sure}}, [](Result<Answers>) { FAIL("must not be called"); });
  }
  CHECK(inner.calls[0].cancelled);
  inner.release();
  inner.pump();  // nothing reaches the destroyed wrapper
}
