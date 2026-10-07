// Flows, offline. Ports tests/test_flows.py: a scripted provider answers each operator by name
// with a fixed value and confidence, and records every call.
#include <map>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "scripted.hpp"

using namespace semop;

namespace {

const Operator department{"department", Choice{"Which team?", {"billing", "technical"}}.with_min_confidence(0.8)};
const Operator urgency{"urgency", Score{"How urgent?", {"low", "medium", "high"}}};
const Operator outage{"outage", Boolean{"Is something down?"}};

using Script = std::map<std::string, std::map<std::string, std::pair<AnswerValue, double>>>;
const Script script = {
    {"charged twice", {{"department", {std::string("billing"), 0.95}}, {"urgency", {1.0, 1}}}},
    {"site down", {{"department", {std::string("technical"), 0.9}}, {"urgency", {2.0, 1}}, {"outage", {true, 0.97}}}},
    {"export broken",
     {{"department", {std::string("technical"), 0.9}}, {"urgency", {1.0, 1}}, {"outage", {false, 0.9}}}},
    {"hmm", {{"department", {std::string("billing"), 0.6}}, {"urgency", {0.0, 1}}}},
};

scripted::Provider make_provider() {
  return scripted::Provider("Scripted", [](const Json& state, const Questions& questions) {
    Answers answers;
    for (const auto& [name, q] : questions) {
      const auto& [value, p] = script.at(state.get<std::string>()).at(name);
      answers.add(name, scripted::answer(q, value, p));
    }
    return Result<Answers>(answers);
  });
}

Outcome<std::string> triage(Ask& ask, const std::string& message) {
  auto a = ask(message, department, urgency);
  auto team = a[department];
  if (!team) return team.stop();
  if (*team == "technical") {
    auto down = ask(message, outage)[outage];
    if (!down) return down.stop();
    if (*down) return "page on-call";
  }
  auto level = a[urgency];
  if (!level) return level.stop();
  return *team + "/" + (*level >= 1.5 ? "urgent" : "normal");
}

}  // namespace

TEST_CASE("a flow returns its value with a trace") {
  auto provider = make_provider();
  auto result = run_flow(provider, triage, std::string("charged twice"));
  CHECK(result.value == "billing/normal");
  CHECK(result.decided());
  CHECK_FALSE(result.stopped_at);
  CHECK_FALSE(result.error);
  CHECK(result.calls() == 1);
  CHECK(result.trace[0].state == "charged twice");
  CHECK(result.trace[0].answers.at("department").confidence == 0.95);
}

TEST_CASE("later steps only run when needed") {
  auto provider = make_provider();
  CHECK(run_flow(provider, triage, std::string("site down")).value == "page on-call");
  CHECK(run_flow(provider, triage, std::string("export broken")).value == "technical/normal");
  std::vector<std::pair<std::string, std::vector<std::string>>> calls;
  for (const auto& c : provider.calls) calls.emplace_back(c.state.get<std::string>(), c.names);
  CHECK(calls == std::vector<std::pair<std::string, std::vector<std::string>>>{
                     {"site down", {"department", "urgency"}},
                     {"site down", {"outage"}},
                     {"export broken", {"department", "urgency"}},
                     {"export broken", {"outage"}}});
}

TEST_CASE("an undecided answer the flow uses stops it") {
  auto provider = make_provider();
  auto result = run_flow(provider, triage, std::string("hmm"));
  CHECK_FALSE(result.value);
  CHECK_FALSE(result.decided());
  CHECK(result.stopped_at == "department");
  CHECK(result.calls() == 1);
  CHECK_FALSE(result.trace[0].answers.at("department").decided());
}

TEST_CASE("an undecided answer the flow never reads doesn't stop it") {
  auto provider = make_provider();
  auto result = run_flow(provider, [](Ask& ask, const std::string& message) -> Outcome<double> {
    auto level = ask(message, department, urgency)[urgency];
    if (!level) return level.stop();
    return *level;
  }, std::string("hmm"));
  CHECK(result.value == 0.0);
}

TEST_CASE("a flow can handle undecided itself") {
  auto provider = make_provider();
  auto result = run_flow(provider, [](Ask& ask, const std::string& message) -> Outcome<std::string> {
    auto a = ask(message, department);
    auto team = a[department];
    if (team) return *team;
    const Answer* full = a.full("department");
    return "human (leaning " + full->probabilities[0].first + ")";
  }, std::string("hmm"));
  CHECK(result.decided());
  CHECK(result.value == "human (leaning billing)");
}

TEST_CASE("full answers are available without stopping") {
  auto provider = make_provider();
  auto result = run_flow(provider, [](Ask& ask, const std::string& message) -> Outcome<double> {
    return ask(message, department).full("department")->confidence;
  }, std::string("hmm"));
  CHECK(result.value == 0.6);
}

TEST_CASE("a provider error ends the flow whatever it returns, and nothing more is sent") {
  FunctionProvider down("Down", [](const Json&, const Questions&) -> Result<Answers> {
    Error e;
    e.kind = ErrorKind::provider_error;
    e.message = "outage";
    return e;
  });
  int later_calls = 0;
  auto result = run_flow(down, [&](Ask& ask, const std::string& message) -> Outcome<std::string> {
    auto team = ask(message, department)[department];
    CHECK(team.failed());
    auto again = ask(message, outage);  // not sent
    later_calls += again.failed();
    return "human";                     // would hide the outage; the error wins
  }, std::string("charged twice"));
  CHECK_FALSE(result.value);
  REQUIRE(result.error);
  CHECK(result.error->message == "Down: outage");
  CHECK(later_calls == 1);
  CHECK(result.calls() == 0);
}

TEST_CASE("bench::run_flow counts right, wrong, and stopped") {
  auto provider = make_provider();
  const std::vector<std::pair<std::string, std::string>> cases = {{"charged twice", "billing/normal"},
                                                                  {"site down", "page on-call"},
                                                                  {"export broken", "technical/urgent"},
                                                                  {"hmm", "billing/normal"}};
  auto report = bench::run_flow(triage, provider, cases);
  CHECK(report.total() == 4);
  CHECK(report.correct() == 2);
  CHECK(report.stopped() == 1);
  CHECK(report.failed() == 0);
  CHECK(report.accuracy_when_answered() == Catch::Approx(2.0 / 3.0));
  CHECK(report.calls() == 6);  // two cases needed the outage step
}
