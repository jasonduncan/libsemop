// Built with the caller's default flags against the packaged libraries: the core with a custom
// provider and a flow, and the TypeSafe provider linked through libtypesafe and libcurl.
#include <iostream>

#include <semop/semop.hpp>
#include <semop/typesafe.hpp>

namespace {

const semop::Operator yes{"yes", semop::Boolean{"Is it yes?"}};

semop::Outcome<std::string> flow(semop::Ask& ask, const std::string& text) {
  auto v = ask(text, yes)[yes];
  if (!v) return v.stop();
  return *v ? "yes" : "no";
}

}  // namespace

int main() {
  // A custom provider, subclassed from outside the library.
  semop::FunctionProvider rules("Rules", [](const semop::Json& state, const semop::Questions& qs) {
    const double p = state == "yes please" ? 0.9 : 0.1;
    semop::Answers answers;
    answers.add(qs[0].name, semop::make_answer(qs[0].question, p > 0.5, {{"true", p}, {"false", semop::complement(p)}}).value());
    return semop::Result<semop::Answers>(answers);
  });
  auto result = semop::run_flow(rules, flow, std::string("yes please"));
  if (result.value != "yes") return 1;

  // The TypeSafe provider links (no call is made).
  libtypesafe::ClientOptions options;
  options.api_key = "consumer-test-key";
  auto client = libtypesafe::Client::create(options);
  if (!client) return 1;
  semop::TypeSafe provider(*client);
  std::cout << "libsemop " << semop::version << " OK (provider " << provider.name() << ")\n";
  return 0;
}
