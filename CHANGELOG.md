# Changelog

## Unreleased

- Images (TypeSafe "Dreamer-ready"): `semop::Image` (`from_bytes` / `from_file` / `from_base64`),
  checked client-side: PNG/JPEG only, no animated PNG, 16 MiB per file, 256-1568 px per side,
  78,400-2,458,624 px area; reports EXIF orientation and progressive JPEGs. Placeholders
  (`image-<uuid>`) go anywhere text goes; `frames()` builds a multi-frame state array.
- `semop::Box` question and `Region` answer value (`top`/`bottom`/`left`/`right` fractions plus
  confidence), `make_box_answer`, `Answer::region()`. Box needs `jev-1.14.0`.
- `semop::locate`: a presence Boolean plus a Box in one call (Box has no "not found").
- `TypeSafe::attach` / `detach`: referenced images are sent as multipart `assets` through
  libtypesafe (request JSON byte-identical to the Python provider's), with
  `TypeSafe-Danger-Zone-Images: true` on every image or Box request; the 32 MiB per-request
  limit (counting each use) is checked before sending. Box answers use libtypesafe's typed
  `BoxAnswer`. Needs libtypesafe v0.2.1, which the build now fetches.
- `wire::encode` reports Box answers as `"type": "box"` with the region as the value.
- `examples/image.cpp`.

## 0.1.0 (unreleased)

First version: a C++17 port of semantic-operators-lib 0.11.0 (Python).

- Questions (`Boolean`, `Choice`, `Score`, `EnumChoice<E>`), `Answer`, `Call`, and `make_answer`
  with the Python library's rules for ties, `min_confidence`, rounding, and validation
- Typed `Operator<Q>`; `ask`, `apply`
- Non-blocking `Provider` (`start` / `pump` / `cancel`), `BlockingProvider`, `FunctionProvider`
- Flows without exceptions: `run_flow`, `Ask`, `Value<T>`, `Outcome<T>`, `FlowResult<T>`
- `filter_items`, `rerank`, `reweighted`, `ask_each` (concurrent, one call per item)
- `Cascade` (escalation) and `TimeLimited`
- `bench`: `run`, `score`, `at_min_confidence`, `stability`, `ndcg`, `run_flow`
- `wire`: the `semop` JSON request and response (schema_version "1")
- `semop::TypeSafe` provider (target `libsemop::typesafe`) on libtypesafe 0.1; requests are
  byte-identical to the Python provider's
- CMake package `libsemop` with targets `libsemop::semop` and `libsemop::typesafe`
