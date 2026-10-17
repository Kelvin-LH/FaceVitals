# Third-party components

The root MIT license covers this project's original code. It does not replace upstream licenses.

| Component | License and source |
|---|---|
| Qt 6, dynamically linked | LGPL/GPL/commercial terms applicable to the build. The Windows package includes licenses/Qt.txt. [Qt licensing](https://www.qt.io/licensing/open-source-lgpl-obligations). Users may replace the shared libraries with compatible builds. Source build instructions are in README.md. |
| ONNX Runtime 1.23.2 | MIT; release packages include LICENSE and ThirdPartyNotices.txt. [Source](https://github.com/microsoft/onnxruntime/tree/v1.23.2). |
| YuNet detector | MIT; models/bootstrap/FACE_LICENSE.txt and PROVENANCE.md. |
| EmotiEffLib pretrained emotion model | Upstream Apache-2.0 notice in models/bootstrap/EMOTION_LICENSE.txt. See PROVENANCE.md for exact weight source and hash. Training data and weight usage conditions need separate review for the intended use. No target-population validation is claimed. |
| astronaut.png test fixture | NASA public-domain image distributed by scikit-image; tests/fixtures/README.md. |
| MSVC runtime and other packaged dependencies | Their redistribution terms and package licenses apply. |

The public MPU research sample video and contact-sensor CSV are not redistributed. The replay tool requires independently obtained source files. No application databases, credentials, personal recordings or raw participant signals are part of this repository or release.
