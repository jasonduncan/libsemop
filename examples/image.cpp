// Ask about a photo and find a thing in it (TypeSafe, Jev 1.14).
//   example_image kitchen.jpg
// Image support is TypeSafe "Dreamer-ready" (not production-ready). Needs TYPESAFE_API_KEY.
// The image must be PNG/JPEG, 256-1568 px per side, with upright pixels: resize and rotate first.
#include <iostream>

#include <semop/semop.hpp>
#include <semop/typesafe.hpp>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: example_image <photo.png|jpg>\n";
    return 2;
  }
  auto photo = semop::Image::from_file(argv[1]);
  if (!photo) {
    std::cerr << photo.error().message << '\n';
    return 2;
  }
  if (photo->exif_orientation()) std::cerr << "warning: EXIF rotation is ignored; rotate the pixels yourself\n";

  auto client = libtypesafe::Client::create();
  if (!client) {
    std::cerr << client.error().message << '\n';
    return 2;
  }
  semop::TypeSafe provider(*client, "jev-1.14.0");  // Box needs 1.14
  provider.attach(*photo);
  const semop::Json state = {{"photo", photo->placeholder()}};

  // Options are a name -> description map; descriptions may hold image placeholders too.
  const semop::Operator indoors{"indoors", semop::Boolean{"Was `photo` taken indoors?"}};
  const semop::Operator room{"room", semop::Choice{"Which room is `photo`?",
                                                   {{"kitchen", "Has a stove, sink or fridge."},
                                                    {"office", "Has a desk or computer."},
                                                    {"other"}}}};
  auto answers = semop::apply(provider, state, {indoors, room});
  if (!answers) {
    std::cerr << answers.error().message << '\n';
    return 1;
  }
  std::cout << "indoors: " << answers->value(indoors).value_or(false) << '\n';
  std::cout << "room: " << answers->value(room).value_or("(not sure)") << '\n';

  // Box never says "not found", so locate() asks a presence question with it, in one call.
  auto mug = semop::locate(provider, state, "a coffee mug", {"`photo`"});
  if (!mug) {
    std::cerr << mug.error().message << '\n';
    return 1;
  }
  if (mug->region) {
    const auto px = mug->region->pixels(photo->width(), photo->height());
    std::cout << "mug at (" << px.left << ", " << px.top << ")-(" << px.right << ", " << px.bottom
              << "), box confidence " << mug->box.confidence << '\n';
  } else {
    std::cout << (mug->found == false ? "no mug\n" : "not sure there's a mug\n");
  }
}
