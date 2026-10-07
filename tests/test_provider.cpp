// The Provider contract: delivery only in pump(), cancellation, validation, answer checks,
// and the blocking helpers. Offline.
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "scripted.hpp"

using namespace semop;
using Catch::Matchers::ContainsSubstring;

namespace {

const Operator urgent{"urgent", Boolean{"Is it urgent?"}};
const Operator topic{"topic", Choice{"What topic?", {"billing", "technical"}}};

/// Answers every question confidently: true, the first option, or level 0.
Result<Answers> confident(const Json&, const Questions& questions) {
  Answers answers;
  auto call = std::make_shared<const Call>(Call{"Fake", "fake-1", 10, 1});
  for (const auto& [name, q] : questions) {
    if (std::holds_alternative<Boolean>(q)) answers.add(name, scripted::answer(q, true, 0.9, call));
    if (const auto* c = std::get_if<Choice>(&q)) answers.add(name, scripted::answer(q, c->options[0].name, 0.9, call));
    if (std::holds_alternative<Score>(q)) answers.add(name, scripted::answer(q, 0.0, 1.0, call));
  }
  return answers;
}

}  // namespace

TEST_CASE("start never calls done itself; pump delivers") {
  scripted::Provider provider("Fake", confident);
  int delivered = 0;
  provider.start("hi", Questions{urgent}, [&](Result<Answers> r) {
    CHECK(r);
    ++delivered;
  });
  CHECK(delivered == 0);
  CHECK(provider.in_flight() == 1);
  provider.pump();
  CHECK(delivered == 1);
  CHECK(provider.in_flight() == 0);
}

TEST_CASE("cancel stops a call and suppresses its done") {
  scripted::Provider provider("Fake", confident);
  provider.hold = true;
  bool called = false;
  const Ticket t = provider.start("hi", Questions{urgent}, [&](Result<Answers>) { called = true; });
  provider.cancel(t);
  CHECK(provider.calls[0].cancelled);
  provider.release();
  provider.pump();
  CHECK_FALSE(called);
}

TEST_CASE("invalid questions come back as invalid_question without a call") {
  scripted::Provider provider("Fake", confident);
  auto r = ask(provider, "hi", Questions{{"x", Choice{"Which?", {"only"}}}});
  REQUIRE_FALSE(r);
  CHECK(r.error().kind == ErrorKind::invalid_question);
  CHECK(r.error().message == "x: need at least 2 options, got 1");
  CHECK(provider.calls.empty());
}

TEST_CASE("answers are checked against the questions and put in question order") {
  SECTION("reordered") {
    scripted::Provider provider("Fake", [](const Json& s, const Questions& qs) {
      auto answers = confident(s, qs);
      Answers reversed;
      std::vector<std::pair<std::string, Answer>> all(answers->begin(), answers->end());
      for (auto it = all.rbegin(); it != all.rend(); ++it) reversed.add(it->first, it->second);
      return Result<Answers>(reversed);
    });
    auto r = apply(provider, "hi", {urgent, topic});
    REQUIRE(r);
    CHECK(r->begin()->first == "urgent");
  }
  SECTION("missing") {
    scripted::Provider provider("Fake", [](const Json&, const Questions&) { return Result<Answers>(Answers{}); });
    auto r = ask(provider, "hi", Questions{urgent});
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == ErrorKind::provider_error);
    CHECK(r.error().provider == "Fake");
    CHECK(r.error().message == "Fake: unexpected response: no answer for 'urgent'");
  }
  SECTION("extra") {
    scripted::Provider provider("Fake", [](const Json& s, const Questions& qs) {
      auto answers = confident(s, qs);
      answers->add("surprise", answers->at("urgent"));
      return answers;
    });
    auto r = ask(provider, "hi", Questions{urgent});
    REQUIRE_FALSE(r);
    CHECK_THAT(r.error().message, ContainsSubstring("weren't asked"));
  }
}

TEST_CASE("blocking helpers: ask, apply, one operator") {
  FunctionProvider provider("Fake", confident);
  auto one = ask(provider, "hi", urgent);
  REQUIRE(one);
  CHECK(one->boolean() == true);
  CHECK(one->call->model == "fake-1");
  auto both = apply(provider, "hi", {urgent, topic});
  REQUIRE(both);
  CHECK(both->value(topic) == "billing");
  CHECK(both->size() == 2);
}

TEST_CASE("FunctionProvider names its provider errors") {
  FunctionProvider provider("Rules", [](const Json&, const Questions&) -> Result<Answers> {
    Error e;
    e.kind = ErrorKind::provider_error;
    e.message = "no rule matched";
    return e;
  });
  auto r = ask(provider, "hi", urgent);
  REQUIRE_FALSE(r);
  CHECK(r.error().provider == "Rules");
  CHECK(r.error().message == "Rules: no rule matched");
  CHECK(r.error().is_provider_error());
}

TEST_CASE("blocking calls are refused inside a callback instead of deadlocking") {
  FunctionProvider provider("Fake", confident);
  bool refused = false;
  provider.start("hi", Questions{urgent}, [&](Result<Answers>) {
    auto nested = ask(provider, "again", urgent);
    refused = !nested && nested.error().kind == ErrorKind::invalid_argument;
  });
  provider.pump();
  CHECK(refused);
}

TEST_CASE("a provider may finish from another thread; delivery stays on the pumping thread") {
  class Threaded : public semop::Provider {
   public:
    Threaded() : semop::Provider("Threaded") {}
    std::vector<std::thread> workers;
    ~Threaded() override {
      for (auto& w : workers) w.join();
    }

   protected:
    void start_call(Ticket ticket, const Json& state, const Questions& questions) override {
      workers.emplace_back([this, ticket, state, questions] { finish(ticket, confident(state, questions)); });
    }
  };
  Threaded provider;
  const auto pumping_thread = std::this_thread::get_id();
  std::thread::id delivered_on;
  auto r = ask(provider, "hi", urgent);
  REQUIRE(r);
  provider.start("hi", Questions{urgent}, [&](Result<Answers>) { delivered_on = std::this_thread::get_id(); });
  for (auto& w : provider.workers) w.join();
  provider.workers.clear();
  provider.pump();
  CHECK(delivered_on == pumping_thread);
}
