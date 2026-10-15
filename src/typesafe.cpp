#include "semop/typesafe.hpp"

#include <algorithm>
#include <thread>

#include "util.hpp"

namespace semop {

namespace ts = libtypesafe;

namespace {

Json optional_text(const std::optional<std::string>& text) { return text ? Json(*text) : Json(nullptr); }

}  // namespace

// Sent exactly as the Python provider sends them through the official SDK.
Json typesafe_question(const Question& question) {
  if (const auto* b = std::get_if<Boolean>(&question)) {
    Json q = {{"type", "noul"}, {"instructions", b->instructions}};
    if (b->yes || b->no) q["criteria"] = {{"true", optional_text(b->yes)}, {"false", optional_text(b->no)}};
    return q;
  }
  if (const auto* c = std::get_if<Choice>(&question)) {
    Json criteria = Json::object();
    for (const auto& option : c->options) criteria[option.name] = optional_text(option.description);
    return {{"type", "choice"}, {"instructions", c->instructions}, {"criteria", std::move(criteria)}};
  }
  if (const auto* box = std::get_if<Box>(&question)) return {{"type", "box"}, {"instructions", box->instructions}};
  const auto& s = std::get<Score>(question);
  return {{"type", "score"}, {"instructions", s.instructions}, {"criteria", s.levels}};
}

void TypeSafe::attach(const Image& image) { images_.insert_or_assign(image.placeholder(), image); }

void TypeSafe::detach(const Image& image) { images_.erase(image.placeholder()); }

TypeSafe::TypeSafe(ts::Client& client, std::string model, ts::CallOptions options)
    : Provider("TypeSafe"), client_(client), model_(std::move(model)), options_(std::move(options)) {}

// Dropping the handles cancels the calls and suppresses their callbacks, which capture `this`.
TypeSafe::~TypeSafe() { pending_.clear(); }

void TypeSafe::start_call(Ticket ticket, const Json& state, const Questions& questions) {
  ts::Questions request;
  for (const auto& nq : questions) request.add_raw(nq.name, typesafe_question(nq.question));
  ts::CallOptions options = options_;
  options.model = model_;

  // Images: the attached ones this call references go in `assets`, each once.
  std::vector<std::string> used = image_placeholders(state);
  for (auto& placeholder : image_placeholders(questions)) used.push_back(std::move(placeholder));
  Json assets = Json::object();
  std::size_t unique_bytes = 0;
  std::size_t expanded_bytes = 0;
  for (const auto& placeholder : used) {
    const auto it = images_.find(placeholder);
    if (it == images_.end()) {
      finish(ticket, detail::invalid(ErrorKind::invalid_argument,
                                     placeholder + " is not an attached image; call TypeSafe::attach(image) first"));
      return;
    }
    expanded_bytes += it->second.size();
    if (!assets.contains(placeholder)) {
      unique_bytes += it->second.size();
      assets[placeholder] = {{"type", "image"}, {"format", it->second.format_name()}, {"data_base64", it->second.base64()}};
    }
  }
  if (unique_bytes > image_limits::max_request_bytes || expanded_bytes > image_limits::max_request_bytes) {
    finish(ticket, detail::invalid(ErrorKind::invalid_argument,
                                   "images in one request must total at most 32 MiB, counting each use; send fewer or smaller images"));
    return;
  }
  const bool has_box = std::any_of(questions.begin(), questions.end(),
                                   [](const NamedQuestion& nq) { return std::holds_alternative<Box>(nq.question); });
  if (!assets.empty() || has_box) {
    options.headers.set(typesafe_images_header, "true");
    if (!options.extra_body.is_object()) options.extra_body = Json::object();
    options.extra_body["assets"] = std::move(assets);
  }

  auto decode = [this, questions](ts::Result<ts::SystemOneResult> result) -> Result<Answers> {
    if (!result) {
      Error error = provider_error(result.error().message);
      if (result.error().kind == ts::ErrorKind::timeout) error.kind = ErrorKind::timeout;
      return error;
    }
    const ts::SystemOneResult& response = *result;
    auto call = std::make_shared<const Call>(
        Call{"TypeSafe", response.model(), response.usage().input_tokens, response.usage().output_tokens});
    const Json body = Json::parse(response.meta().body, nullptr, /*allow_exceptions=*/false);
    const auto raw_answer = [&](const std::string& name) -> Json {
      if (!body.is_object() || !body.contains("answers") || !body["answers"].contains(name)) return nullptr;
      return body["answers"][name];
    };
    const auto unexpected = [this](const std::string& what) { return provider_error("unexpected response: " + what); };

    Answers answers;
    for (const auto& [name, question] : questions) {
      const ts::Answer* answer = response.find(name);
      if (answer == nullptr) return unexpected("no answer for '" + name + "'");
      Result<Answer> built = Error{};
      if (std::holds_alternative<Boolean>(question)) {
        const auto* noul = std::get_if<ts::NoulAnswer>(answer);
        if (noul == nullptr) return unexpected("'" + name + "' is not a yes/no answer");
        // TypeSafe returns one number: the probability the answer is "true".
        const double p = noul->noul;
        built = make_answer(question, AnswerValue(p > 0.5), {{"true", p}, {"false", complement(p)}}, raw_answer(name), call);
      } else if (const auto* c = std::get_if<Choice>(&question)) {
        const auto* choice = std::get_if<ts::ChoiceAnswer>(answer);
        if (choice == nullptr) return unexpected("'" + name + "' is not a choice answer");
        std::vector<std::pair<std::string, double>> probabilities;
        for (const auto& option : c->options) {
          for (const auto& [label, p] : choice->probabilities) {
            if (label == option.name) probabilities.emplace_back(label, p);
          }
        }
        built = make_answer(question, AnswerValue(choice->choice), std::move(probabilities), raw_answer(name), call);
      } else if (const auto* box = std::get_if<Box>(&question)) {
        // libtypesafe 0.1 doesn't model box answers (they arrive as UnknownAnswer); read the raw JSON.
        const Json raw = raw_answer(name);
        const auto edge = [&](const char* key, double& out) {
          if (!raw.is_object() || !raw.contains("box") || !raw["box"].is_object()) return false;
          const Json& b = raw["box"];
          if (!b.contains(key) || !b[key].is_number()) return false;
          out = b[key].get<double>();
          return true;
        };
        Region region;
        if (!raw.is_object() || raw.value("type", "") != "box" || !edge("top", region.top) ||
            !edge("bottom", region.bottom) || !edge("left", region.left) || !edge("right", region.right)) {
          return unexpected("'" + name + "' is not a box answer");
        }
        std::optional<double> confidence;
        if (raw.contains("confidence") && raw["confidence"].is_number()) confidence = raw["confidence"].get<double>();
        built = make_box_answer(*box, region, confidence, raw, call);
      } else {
        const auto& s = std::get<Score>(question);
        const auto* score = std::get_if<ts::ScoreAnswer>(answer);
        if (score == nullptr) return unexpected("'" + name + "' is not a score answer");
        // TypeSafe keys probabilities by level index; ours are keyed by level text.
        std::vector<std::pair<std::string, double>> probabilities;
        for (std::size_t i = 0; i < s.levels.size() && i < score->probabilities.size(); ++i) {
          probabilities.emplace_back(s.levels[i], score->probabilities[i]);
        }
        built = make_answer(question, AnswerValue(score->score), std::move(probabilities), raw_answer(name), call);
      }
      if (!built) return unexpected(built.error().message);
      answers.add(name, std::move(built).value());
    }
    return answers;
  };

  auto pending = client_.system_one_async(
      state, std::move(request), std::move(options),
      [this, ticket, decode = std::move(decode)](ts::Result<ts::SystemOneResult> result) {
        pending_.erase(ticket);
        finish(ticket, decode(std::move(result)));
      });
  pending_.emplace(ticket, std::move(pending));
}

void TypeSafe::cancel_call(Ticket ticket) { pending_.erase(ticket); }

void TypeSafe::on_pump() { client_.pump(); }

void TypeSafe::wait(std::chrono::milliseconds max_wait) {
  const auto nap = std::clamp(max_wait, std::chrono::milliseconds(0), std::chrono::milliseconds(1));
  if (nap.count() > 0) std::this_thread::sleep_for(nap);
}

}  // namespace semop
