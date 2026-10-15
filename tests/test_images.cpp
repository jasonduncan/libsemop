// Images, Box answers, and locate. Offline.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <semop/semop.hpp>

#include "imagekit.hpp"

using namespace semop;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("PNG and JPEG sizes are read") {
  auto png = Image::from_bytes(imagekit::png(300, 400));
  REQUIRE(png);
  CHECK(png->format() == ImageFormat::png);
  CHECK(png->width() == 300);
  CHECK(png->height() == 400);
  CHECK(png->placeholder().size() == 42);
  CHECK(png->placeholder().rfind("image-", 0) == 0);
  auto jpeg = Image::from_bytes(imagekit::jpeg(1568, 1000));
  REQUIRE(jpeg);
  CHECK(jpeg->format_name() == "jpeg");
  CHECK(jpeg->width() == 1568);
  CHECK_FALSE(jpeg->progressive());
  CHECK_FALSE(jpeg->exif_orientation());
}

TEST_CASE("bad images are rejected with a clear message") {
  const auto rejects = [](imagekit::Bytes bytes, const std::string& message) {
    auto image = Image::from_bytes(std::move(bytes));
    REQUIRE_FALSE(image);
    CHECK(image.error().kind == ErrorKind::invalid_argument);
    CHECK_THAT(image.error().message, ContainsSubstring(message));
  };
  rejects({'G', 'I', 'F', '8', '9', 'a', 0, 0, 0, 0}, "PNG or JPEG");
  rejects(imagekit::png(255, 400), "width is 255 px");
  rejects(imagekit::png(400, 1569), "height is 1569 px");
  rejects(imagekit::png(256, 256), "area is 65536");
  rejects(imagekit::png(280, 280, true), "animated");
  rejects(imagekit::jpeg(100, 300), "width is 100 px");
  rejects({0xFF, 0xD8, 0xFF, 0xD9}, "malformed JPEG");
  auto big = imagekit::png();
  big.resize(image_limits::max_file_bytes + 1);
  rejects(big, "16 MiB");
  CHECK(Image::from_bytes(imagekit::jpeg(1568, 1568)));  // exactly the max area
  CHECK(Image::from_bytes(imagekit::png(280, 280)));     // exactly the min area
}

TEST_CASE("EXIF rotation and progressive JPEGs are reported") {
  auto rotated = Image::from_bytes(imagekit::jpeg(300, 300, 0xC0, 6));
  REQUIRE(rotated);
  CHECK(rotated->exif_orientation() == 6);
  auto progressive = Image::from_bytes(imagekit::jpeg(300, 300, 0xC2));
  REQUIRE(progressive);
  CHECK(progressive->progressive());
}

TEST_CASE("base64 round trip") {
  auto image = Image::from_bytes(imagekit::png());
  REQUIRE(image);
  auto again = Image::from_base64(image->base64());
  REQUIRE(again);
  CHECK(again->bytes() == image->bytes());
  CHECK(again->placeholder() != image->placeholder());
  CHECK_FALSE(Image::from_base64("data:image/png;base64," + image->base64()));
  CHECK_FALSE(Image::from_base64("not base64!"));
  CHECK_FALSE(Image::from_file("/nonexistent/x.png"));
}

TEST_CASE("frames and placeholders") {
  auto a = Image::from_bytes(imagekit::png()).value();
  auto b = Image::from_bytes(imagekit::png()).value();
  auto f = frames({a, b}, {2, 0});
  REQUIRE(f);
  CHECK((*f)[0]["seconds_ago"] == 2.0);
  CHECK((*f)[0]["frame"] == a.placeholder());
  CHECK((*f)[1]["frame"] == b.placeholder());
  CHECK(frames({a})->dump() == R"([{"frame":")" + a.placeholder() + R"("}])");
  CHECK_FALSE(frames({a}, {1, 2}));
  CHECK_FALSE(frames({}));

  const Json state = {{"photo", a.placeholder()}, {"note", "compare with " + b.placeholder() + "."}};
  CHECK(image_placeholders(state) == std::vector<std::string>{a.placeholder(), b.placeholder()});
  const Questions qs{{"brand", Choice{"Which logo?", {{"a", "this one: " + a.placeholder()}, {"b"}}}}};
  CHECK(image_placeholders(qs) == std::vector<std::string>{a.placeholder()});
  CHECK(image_placeholders(Json("image-not-a-uuid")).empty());
}

TEST_CASE("Box answers carry a Region") {
  const Box q{"the mug", 0.5};
  auto answer = make_box_answer(q, Region{0.2, 0.6, 0.1, 0.5}, 0.9);
  REQUIRE(answer);
  CHECK(answer->region() == Region{0.2, 0.6, 0.1, 0.5});
  CHECK(answer->confidence == 0.9);
  CHECK_FALSE(make_box_answer(q, Region{0.2, 0.6, 0.1, 0.5}, 0.4)->decided());
  CHECK(make_box_answer(Box{"x"}, Region{0, 1, 0, 1}, std::nullopt)->confidence == 0);
  CHECK_FALSE(make_box_answer(q, Region{0.6, 0.2, 0, 1}, 0.9));
  CHECK_FALSE(make_answer(q, AnswerValue(true), {}));
  const Region r{0.2, 0.6, 0.1, 0.5};
  const auto px = r.pixels(1000, 500);
  CHECK((px.left == 100 && px.top == 100 && px.right == 500 && px.bottom == 300));
  CHECK(r.padded(1.0) == Region{0, 1, 0, 0.9});
  const Operator mug{"mug", Box{"the mug"}};
  CHECK(check(Questions{mug}) == std::nullopt);
}

TEST_CASE("locate pairs a Box with a presence check") {
  const auto run = [](double p_present) {
    FunctionProvider provider("Fake", [p_present](const Json&, const Questions& qs) -> Result<Answers> {
      CHECK(std::get<Boolean>(qs[0].question).instructions == "Is there a coffee mug in `photo`?");
      CHECK(std::get<Box>(qs[1].question).instructions == "a coffee mug in `photo`");
      Answers answers;
      answers.add("present", make_answer(qs[0].question, AnswerValue(p_present > 0.5),
                                         {{"true", p_present}, {"false", complement(p_present)}})
                                 .value());
      answers.add("box", make_box_answer(std::get<Box>(qs[1].question), Region{0.1, 0.5, 0.5, 0.9}, 0.8).value());
      return answers;
    });
    return locate(provider, Json{{"photo", "x"}}, "a coffee mug", {"`photo`"}).value();
  };
  const auto yes = run(0.9);
  CHECK(yes.found == true);
  CHECK(yes.region == Region{0.1, 0.5, 0.5, 0.9});
  const auto no = run(0.1);
  CHECK(no.found == false);
  CHECK_FALSE(no.region);
  const auto unsure = run(0.5);
  CHECK_FALSE(unsure.found);
  CHECK_FALSE(unsure.region);
}
