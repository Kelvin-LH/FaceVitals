# 构建依赖与分发说明

依赖版本记录：2026-10-02；Windows 0.6 重新构建：2026-10-17。Linux 0.6 尚未复测。

| 组件 | 本次版本 | 用途 / 来源 |
|---|---|---|
| C++ | C++17 | MSVC 19.44 / GCC 15.2 |
| Qt | Windows 6.10.0；Linux 6.10.2 | Core、Gui、Widgets、Sql、Network；测试另用 Test。https://www.qt.io/ |
| ONNX Runtime | 1.23.2 CPU | https://github.com/microsoft/onnxruntime/releases/tag/v1.23.2 |
| YuNet | 2023mar | 232 KiB 检测权重，来源和 SHA-256 见 models/bootstrap/PROVENANCE.md |
| EmotiEffLib | enet_b0_8_best_afew | 8 类表情权重，来源和 SHA-256 见 models/bootstrap/PROVENANCE.md |
| SQLite | Qt qsqlite 插件提供 | 本地账户、模型与测量索引 |
| OS API | Media Foundation / V4L2 | Windows / Linux 摄像头 |

Qt 动态链接，分发许可证位于 Windows 包 licenses/Qt.txt。其适用许可及替换动态库的要求需随正式发行遵守；具体构建的上游来源可从 vcpkg qtbase 的版本记录取得。ONNX Runtime 为 MIT，保留其第三方声明。模型许可证单列，不把代码仓库许可自动视为数据与权重的商用授权。

Windows 包包含应用、所需 Qt 与 MSVC DLL、ORT DLL、两份权重；不含测试程序、PDB、SDK、编译器、Python、CUDA。Linux 包使用目标系统 Qt 动态库，并自带 ORT；本次 Linux 二进制针对 Ubuntu 26.04 构建，不声称可以直接运行在更旧的 glibc 上。其他发行版应从源码构建。

下载 SDK 后分别解压到 external/onnxruntime-win-x64-1.23.2 和 external/onnxruntime-linux-x64-1.23.2。可以通过 ONNXRUNTIME_ROOT 指定其他绝对目录。关闭 FV_ENABLE_ONNX 可编译无深度推理引擎版本，但该版本不能执行真实人脸检测与初始表情模型推理。
