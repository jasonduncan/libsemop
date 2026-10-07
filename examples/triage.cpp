// A flow: route a support message, asking a second question only when it matters.
// "Don't know", or no team that fits, sends the message to a person instead of guessing.
// A forced choice needs an escape option ("other"): without one, the model must pick a team.
// Needs TYPESAFE_API_KEY.
#include <iostream>
#include <string>

#include <semop/semop.hpp>
#include <semop/typesafe.hpp>

namespace {

const semop::Operator department{
    "department",
    semop::Choice{"Which team should handle this?",
                  {{"billing", "Payments, refunds"}, {"technical", "Bugs, errors"}, {"other", "Anything else"}}}
        .with_min_confidence(0.8)};
const semop::Operator urgency{"urgency", semop::Score{"How urgent is this?", {"low", "medium", "high"}}};
const semop::Operator outage{"outage", semop::Boolean{"Is a service down for the customer?"}};

semop::Outcome<std::string> triage(semop::Ask& ask, const std::string& message) {
  auto a = ask(message, department, urgency);  // one call, two operators
  auto team = a[department];
  if (!team || *team == "other") return "human";  // not sure, or no team fits: a person decides
  if (*team == "technical") {
    auto down = ask(message, outage)[outage];   // a second call, only for technical messages
    if (!down) return down.stop();
    if (*down) return "page on-call";
  }
  auto level = a[urgency];
  return *team + (level && *level >= 1.5 ? " (urgent)" : "");
}

}  // namespace

int main() {
  auto client = libtypesafe::Client::create();
  if (!client) {
    std::cerr << client.error().message << '\n';
    return 1;
  }
  semop::TypeSafe provider(*client);
  for (const std::string message : {"Please refund the duplicate charge on my card.",
                                    "Your whole site returns 500 errors, nobody can log in.",
                                    "Hi, just saying hello."}) {
    auto result = semop::run_flow(provider, triage, message);
    std::cout << message << "\n  -> ";
    if (result.error) {
      std::cout << "error: " << result.error->message;
    } else if (!result.decided()) {
      std::cout << "stopped: don't know '" << *result.stopped_at << "'";
    } else {
      std::cout << *result.value;
    }
    std::cout << "  (" << result.calls() << " call" << (result.calls() == 1 ? "" : "s") << ")\n";
  }
}
