// Quick start: three kinds of question in one call. Needs TYPESAFE_API_KEY.
#include <iostream>

#include <semop/semop.hpp>
#include <semop/typesafe.hpp>

int main() {
  auto client = libtypesafe::Client::create();  // you create and own the client
  if (!client) {
    std::cerr << client.error().message << '\n';
    return 1;
  }
  semop::TypeSafe provider(*client);  // model defaults to "jev-latest"

  const semop::Operator complaint{"is_complaint", semop::Boolean{"Is the customer complaining?"}};
  const semop::Operator department{
      "department", semop::Choice{"Which team should handle this?", {{"billing", "Payments, refunds"}, {"other"}}}};
  const semop::Operator urgency{"urgency", semop::Score{"How urgent is this?", {"low", "medium", "high"}}};

  auto answers = semop::apply(provider, "I was charged twice and I'm furious.", {complaint, department, urgency});
  if (!answers) {
    std::cerr << answers.error().message << '\n';
    return 1;
  }
  std::cout << "complaint:  " << answers->value(complaint).value_or(false) << '\n';
  std::cout << "department: " << answers->value(department).value_or("(don't know)") << '\n';
  for (const auto& [option, p] : (*answers)[department].probabilities) std::cout << "  " << option << ' ' << p << '\n';
  std::cout << "urgency:    " << answers->value(urgency).value_or(-1) << " of 0..2\n";
  std::cout << "model:      " << (*answers)[department].call->model.value_or("?") << '\n';
}
