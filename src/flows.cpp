#include "semop/flows.hpp"

namespace semop {

Asked Ask::operator()(const Json& state, const Questions& questions) {
  if (error_) return Asked();  // after a failure, nothing more is sent
  auto answers = ask(provider_, state, questions);
  if (!answers) {
    error_ = answers.error();
    return Asked();
  }
  trace_.push_back(Step{state, *answers});
  return Asked(std::move(answers).value());
}

}  // namespace semop
