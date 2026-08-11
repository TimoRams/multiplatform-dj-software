# Repository Structure Status

The source-layout proposal has been implemented. Production CMake enters through
`src/CMakeLists.txt`, source ownership lives in domain CMake files, and the
current tree is described in `source-structure.md`.

QML resources keep their established `qrc:/DJSoftware/src/qml/...` prefix while
their source files are grouped by UI domain. Linux packaging metadata remains in
its existing location because moving it would require a separate packaging and
release-tooling review.

The root `CMakeLists.txt` still contains explicit test-target declarations. A
future build-only patch may move those declarations verbatim to
`cmake/BrockDJTests.cmake`; it should not simultaneously alter test grouping or
runtime behavior.

Unity builds and a target-wide PCH remain disabled. Earlier evaluation exposed
anonymous-namespace and platform-macro collisions across otherwise valid
translation units. Enable either optimization only after cross-platform timing
data justifies the dependency risk.
