#include "semop/provider.hpp"

#include <algorithm>
#include <thread>

#include "util.hpp"

namespace semop {

Provider::Provider(std::string name) : name_(std::move(name)) {}

// The key function: this file is built with RTTI, like the rest of the library, so subclasses
// compiled with RTTI can link.
Provider::~Provider() = default;

Ticket Provider::start(Json state, Questions questions, Done done) {
  const Ticket ticket = ++next_ticket_;
  Pending pending{std::move(done), {}};
  for (const auto& q : questions) pending.names.push_back(q.name);
  calls_.emplace(ticket, std::move(pending));
  if (auto problem = check(questions)) {
    finish(ticket, std::move(*problem));  // delivered by the next pump(); nothing is sent
    return ticket;
  }
  start_call(ticket, state, questions);
  return ticket;
}

void Provider::cancel(Ticket ticket) {
  if (calls_.erase(ticket) > 0) cancel_call(ticket);
}

void Provider::cancel_call(Ticket) {}
void Provider::on_pump() {}

void Provider::finish(Ticket ticket, Result<Answers> result) {
  std::lock_guard<std::mutex> lock(finished_mutex_);
  finished_.emplace_back(ticket, std::move(result));
}

Error Provider::provider_error(std::string message) const {
  Error error;
  error.kind = ErrorKind::provider_error;
  error.provider = name_;
  error.message = name_ + ": " + message;
  return error;
}

void Provider::pump() {
  if (in_pump_) return;
  in_pump_ = true;
  on_pump();
  {
    std::lock_guard<std::mutex> lock(finished_mutex_);
    delivering_.swap(finished_);
  }
  for (auto& [ticket, result] : delivering_) {
    const auto it = calls_.find(ticket);
    if (it == calls_.end()) continue;  // cancelled
    Pending pending = std::move(it->second);
    calls_.erase(it);
    if (result.ok()) {
      // Exactly the questions asked, in question order.
      Answers ordered;
      std::string missing;
      for (const auto& name : pending.names) {
        if (const Answer* answer = result->find(name)) {
          ordered.add(name, *answer);
        } else if (missing.empty()) {
          missing = name;
        }
      }
      if (!missing.empty()) {
        result = provider_error("unexpected response: no answer for '" + missing + "'");
      } else if (result->size() != pending.names.size()) {
        result = provider_error("unexpected response: answers for questions that weren't asked");
      } else {
        result = std::move(ordered);
      }
    }
    pending.done(std::move(result));
  }
  delivering_.clear();
  in_pump_ = false;
}

void Provider::wait(std::chrono::milliseconds max_wait) {
  const auto nap = std::clamp(max_wait, std::chrono::milliseconds(0), std::chrono::milliseconds(1));
  if (nap.count() > 0) std::this_thread::sleep_for(nap);
}

// ---------------------------------------------------------------------------
// BlockingProvider, FunctionProvider
// ---------------------------------------------------------------------------

void BlockingProvider::wait(std::chrono::milliseconds max_wait) {
  if (queue_.empty()) Provider::wait(max_wait);
}

void BlockingProvider::start_call(Ticket ticket, const Json& state, const Questions& questions) {
  queue_.push_back({ticket, state, questions});
}

void BlockingProvider::cancel_call(Ticket ticket) {
  queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Job& job) { return job.ticket == ticket; }),
               queue_.end());
}

void BlockingProvider::on_pump() {
  std::vector<Job> jobs;
  jobs.swap(queue_);
  for (auto& job : jobs) finish(job.ticket, answer(job.state, job.questions));
}

FunctionProvider::FunctionProvider(std::string name, Fn fn) : BlockingProvider(std::move(name)), fn_(std::move(fn)) {}

Result<Answers> FunctionProvider::answer(const Json& state, const Questions& questions) {
  Result<Answers> result = fn_(state, questions);
  if (!result && result.error().is_provider_error() && result.error().provider.empty()) {
    Error error = result.error();
    error.provider = name();
    error.message = name() + ": " + error.message;
    return error;
  }
  return result;
}

// ---------------------------------------------------------------------------
// Blocking helpers
// ---------------------------------------------------------------------------

Result<Answers> ask(Provider& provider, const Json& state, const Questions& questions) {
  if (provider.pumping()) {
    return detail::invalid(ErrorKind::invalid_argument,
                           "blocking calls can't run inside a provider callback; use Provider::start");
  }
  std::optional<Result<Answers>> out;
  provider.start(state, questions, [&out](Result<Answers> result) { out.emplace(std::move(result)); });
  while (!out) {
    provider.pump();
    if (!out) provider.wait(std::chrono::milliseconds(10));
  }
  return std::move(*out);
}

}  // namespace semop
