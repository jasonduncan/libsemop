// The TypeSafe provider over a real libtypesafe::Client with a scripted transport. Offline.
#include <deque>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <semop/semop.hpp>
#include <semop/typesafe.hpp>

using namespace semop;
using Catch::Matchers::ContainsSubstring;
namespace ts = libtypesafe;

namespace {

/// Records requests; answers each with the next scripted outcome (or holds when there is none).
struct ScriptedTransport : ts::Transport {
  struct Sent {
    ts::TransferId id;
    ts::HttpRequest request;
    std::function<void(ts::TransportOutcome)> done;
    bool finished = false;
    bool cancelled = false;
  };
  std::vector<Sent> sent;
  std::deque<ts::TransportOutcome> script;

  ts::TransferId start(ts::HttpRequest request, std::function<void(ts::TransportOutcome)> done) override {
    sent.push_back({static_cast<ts::TransferId>(sent.size() + 1), std::move(request), std::move(done)});
    return sent.back().id;
  }
  void cancel(ts::TransferId id) override {
    for (auto& s : sent) {
      if (s.id == id && !s.finished) {
        s.finished = s.cancelled = true;
        s.done(ts::TransportError{ts::TransportError::Kind::cancelled, "cancelled", true});
      }
    }
  }
  void poll() override {
    for (auto& s : sent) {
      if (s.finished || script.empty()) continue;
      s.finished = true;
      auto outcome = std::move(script.front());
      script.pop_front();
      s.done(std::move(outcome));
    }
  }
};

ts::HttpResponse ok(const Json& body) {
  return {200, {{"Content-Type", "application/json"}, {"x-typesafe-request-id", "req_1"}}, body.dump()};
}

struct Fixture {
  std::shared_ptr<ScriptedTransport> transport = std::make_shared<ScriptedTransport>();
  ts::Client client = make();

  ts::Client make() {
    ts::ClientOptions options;
    options.api_key = "test-key-123456";
    options.transport = transport;
    options.log_level = ts::LogLevel::off;
    options.retry.max_retries = 0;
    return ts::Client::create(std::move(options)).value();
  }
};

const Operator complaint{"is_complaint", Boolean{"Is the customer complaining?", std::string("They report a problem."), std::nullopt}};
const Operator department{"department", Choice{"Which team?", {{"billing", "Payments, refunds"}, {"technical"}}}};
const Operator urgency{"urgency", Score{"How urgent?", {"low", "medium", "high"}}};

const Json good_body = Json::parse(R"({
  "model": "jev-1.13.0",
  "answers": {
    "is_complaint": {"type": "noul", "noul": 0.07},
    "department": {"type": "choice", "choice": "billing", "confidence": 0.97,
                   "probabilities": {"technical": 0.03, "billing": 0.97}},
    "urgency": {"type": "score", "score": 1.26, "confidence": 0.67,
                "legend": {"0": "low", "1": "medium", "2": "high"},
                "probabilities": {"0": 0.04, "1": 0.66, "2": 0.3}}
  },
  "usage": {"input_tokens": 291, "output_tokens": 20}
})");

}  // namespace

TEST_CASE("questions are sent exactly as the Python provider sends them") {
  CHECK(typesafe_question(complaint.question).dump() ==
        R"({"type":"noul","instructions":"Is the customer complaining?","criteria":{"true":"They report a problem.","false":null}})");
  CHECK(typesafe_question(Boolean{"Is it?"}).dump() == R"({"type":"noul","instructions":"Is it?"})");
  CHECK(typesafe_question(department.question).dump() ==
        R"({"type":"choice","instructions":"Which team?","criteria":{"billing":"Payments, refunds","technical":null}})");
  CHECK(typesafe_question(urgency.question).dump() ==
        R"({"type":"score","instructions":"How urgent?","criteria":["low","medium","high"]})");
}

TEST_CASE("one call, typed answers, and the model that answered") {
  Fixture f;
  TypeSafe provider(f.client, "jev-1.13.0");
  f.transport->script.push_back(ok(good_body));
  auto answers = apply(provider, "I was charged twice and I'm furious.", {complaint, department, urgency});
  REQUIRE(answers);
  REQUIRE(f.transport->sent.size() == 1);
  const Json body = Json::parse(f.transport->sent[0].request.body);
  CHECK(body["model"] == "jev-1.13.0");
  CHECK(body["state"] == "I was charged twice and I'm furious.");
  CHECK(body["questions"]["urgency"]["criteria"] == Json{"low", "medium", "high"});

  // p(true) 0.07: false, with p(false) exactly 0.93.
  CHECK(answers->value(complaint) == false);
  CHECK(answers->at("is_complaint").probability("false") == 0.93);
  CHECK(answers->value(department) == "billing");
  CHECK(answers->at("department").probabilities[0].first == "billing");  // question order
  CHECK(answers->value(urgency) == 1.26);
  CHECK(answers->at("urgency").probability("medium") == 0.66);
  const auto& call = *answers->at("urgency").call;
  CHECK(call.provider == "TypeSafe");
  CHECK(call.model == "jev-1.13.0");
  CHECK(call.input_tokens == 291);
  CHECK(answers->at("department").raw["confidence"] == 0.97);
}

TEST_CASE("the model defaults to jev-latest") {
  Fixture f;
  TypeSafe provider(f.client);
  f.transport->script.push_back(ok(good_body));
  REQUIRE(ask(provider, "hi", urgency));
  CHECK(Json::parse(f.transport->sent[0].request.body)["model"] == "jev-latest");
}

TEST_CASE("failures are provider errors named TypeSafe") {
  SECTION("HTTP 401") {
    Fixture f;
    TypeSafe provider(f.client);
    f.transport->script.push_back(ts::HttpResponse{401, {}, R"({"detail": "Cannot authenticate"})"});
    auto r = ask(provider, "hi", urgency);
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == ErrorKind::provider_error);
    CHECK(r.error().provider == "TypeSafe");
    CHECK_THAT(r.error().message, ContainsSubstring("TypeSafe: GET") || ContainsSubstring("TypeSafe: POST"));
    CHECK_THAT(r.error().message, ContainsSubstring("401 Cannot authenticate"));
  }
  SECTION("timeout") {
    Fixture f;
    TypeSafe provider(f.client);
    f.transport->script.push_back(ts::TransportError{ts::TransportError::Kind::timeout, "slow", true});
    auto r = ask(provider, "hi", urgency);
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == ErrorKind::timeout);
    CHECK(r.error().is_provider_error());
  }
  SECTION("a malformed answer never looks confident") {
    Fixture f;
    TypeSafe provider(f.client);
    Json body = good_body;
    body["answers"]["urgency"]["score"] = 2.9;  // disagrees with its probabilities
    f.transport->script.push_back(ok(body));
    auto r = ask(provider, "hi", urgency);
    REQUIRE_FALSE(r);
    CHECK(r.error().message == "TypeSafe: unexpected response: Score value 2.9 is outside 0..2");
  }
}

TEST_CASE("cancel and destruction cancel the HTTP call") {
  Fixture f;
  {
    TypeSafe provider(f.client);
    bool called = false;
    const Ticket t = provider.start("hi", Questions{urgency}, [&](Result<Answers>) { called = true; });
    provider.cancel(t);
    f.client.pump();
    CHECK(f.transport->sent[0].cancelled);
    provider.start("again", Questions{urgency}, [&](Result<Answers>) { called = true; });
    CHECK_FALSE(called);
  }  // destroyed with a call in flight
  f.client.pump();
  CHECK(f.transport->sent[1].cancelled);
}
