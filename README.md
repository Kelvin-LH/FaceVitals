# FaceVitals 0.6 — C++ 原生桌面识别系统

**研究预览版：心率持续可用性与真人表情准确度尚未达标，详见验证记录。**

Windows / Linux，C++17 + Qt 6 Widgets。首页显示视频、心率、表情分布和信号波形。软件负责采集、识别、录像和结果管理，不承担模型训练。

## 运行

Windows 用户从 [Releases](https://github.com/Kelvin-LH/FaceVitals/releases) 下载 Windows ZIP，解压后运行 Start-FaceVitals.cmd。源码工作区需先完成编译或准备 dist 分发包，再双击根目录 `启动FaceVitals.cmd`，或 `dist/FaceVitals-0.6.0-windows-x64/FaceVitals.exe`。首次运行创建管理员账号，密码至少 10 个字符。后续沿用原数据目录和账号。

1. 首次启动会校验并启用内置表情模型，无需手动导入。已有的活动模型保持不变。
2. 首页填写受试者代号、选择摄像头，点击 **开始测量**。测量持续运行，直到点击 **停止**；没有固定时长上限。
3. 需要保存视频时勾选 **实时录像**。测量过程中可随时开关；每次重新开启生成一个独立片段。
4. 测量完成或点击 **停止** 后自动归档。在 **设置 → 历史与报告** 打开报告、导出报告包或打开录像目录。

知情同意由使用前的流程完成，软件不再设置勾选项。

本次 0.6 发布 Windows 二进制与跨平台源码；Linux 0.6 尚未复测。历史 0.5 Linux 二进制针对 Ubuntu 26.04。安装 `libqt6widgets6 libqt6network6 libqt6sql6-sqlite qt6-qpa-plugins fonts-noto-cjk`，运行分发目录内 `bash run.sh`。摄像头需可通过 `/dev/video*` 访问。

## 设置权限

| 角色 | 账号、自己的历史 | 模型导入 | 模型发布/回滚 | 账号管理、审计 | 采集 |
|---|---|---|---|---|---|
| 普通用户 user | ✓ | — | — | — | ✓ |
| 操作员 operator | ✓ | — | — | — | ✓ |
| 研究员 researcher | ✓ | ✓ | — | — | — |
| 管理员 admin | 全部历史 | ✓ | ✓ | ✓ | ✓ |

模型中心仅向管理员和研究员显示，账号管理与审计仅向管理员显示。模拟信号选项放在管理员/研究员的高级设置中。服务层继续校验权限。所有角色都没有训练页面。

内置模型初始化失败或当前模型损坏时会明确报错，不会继续进行没有表情模型的识别。

## 录像与数据

- 录像默认关闭，保存摄像头原始画面，不录音。格式为 MJPEG AVI，10 fps；按时间补帧以保持播放时长。每 10 分钟或约 512 MiB 自动换段；1 GiB 是单文件保护上限。录像写入失败会提示，识别继续。
- 视频保存在数据目录 `recordings/测量ID/clip-001.avi` 等文件中。完成片段的编号、起始秒数、帧数和 SHA-256 写入测量结果。
- 正常结束、提前停止和关闭窗口时完成录像索引。断电或强制终止可能遗留 `.partial` 文件，不能视为已完成录像。
- 报告包包含带水印 PDF、result.json、完整 signals.csv、完整 emotions.jsonl 和哈希清单；视频通过“打开录像目录”访问，不包含在报告导出包中。
- 默认数据目录为 Qt AppLocalDataLocation；可用 `--data-dir 路径` 指定独立目录。应用内权限不能阻止操作系统账户直接访问未加密的本地文件。

## 摄像头与户外演示

设置中新增摄像头首选模式；Windows 自动在 Media Foundation 与 DirectShow 间回退，支持 OBS 虚拟摄像头。摄像头掉线会自动重连，恢复后重新积累有效信号。

信号连续写盘，内存与报告图保留最近约两分钟；首页只显示 rPPG 与 RGB 四条曲线。相对亮度/色度门槛与共同亮度归一化改善受控光照变化下的信号积累。实机兼容性、设备推荐、现场布置和验证边界见 [摄像头与户外演示指南](摄像头与户外演示指南.md)。

## 识别边界

心率使用 POS 与 0.7–3.5 Hz 正反向滤波，至少积累 20 秒，分析最近至多 30 秒。仅桥接 0.25 秒以内的短缺口，整窗有效覆盖至少 90%，并检查前后半窗频率一致性。界面显示积累进度；短暂失效最多保留 3 秒最近读数，并标注距今时间。离脸、多人、断线和明显位移清空旧读数。质量门槛会拒绝噪声，不能保证所有普通摄像头始终输出。

表情使用 YuNet 与 EmotiEffLib 八类模型、ONNX Runtime CPU 推理，修正长宽比例、轻微倾斜并短时平滑。缺少眼睛/嘴部、曝光异常或分数不足时明确显示质量不足或暂不确定；模型分数未经概率校准。表情标签不能证明人的内心情绪。

**当前普通 RGB 摄像头配置不能可靠测量血压或血氧，两项返回 unsupported。** 没有经过当前相机、光照及人群验证的标定模型，也没有配套参考设备验证。心率不能直接换算血压或 SpO2；可靠测量请使用经过验证的袖带血压计和脉搏血氧仪。呼吸率未实现。所有指标均非医疗诊断，报告保留研究用途水印。

实测与回归范围见 [新版验证记录](validation/v06/README.md)。本项目尚未完成独立生理准确度或多人表情准确度验证。

## 编译与测试

Windows：Visual Studio 2022 C++、CMake、Qt 6，准备 ONNX Runtime 1.23.2 SDK。

```powershell
./scripts/build-windows.ps1 -QtRoot 'Qt根目录' -CMake 'cmake'
./scripts/package-windows.ps1 -QtRoot 'Qt根目录'
```

Linux：安装 `build-essential cmake ninja-build qt6-base-dev libqt6sql6-sqlite libgl1-mesa-dev fonts-noto-cjk` 并准备 ORT SDK。

```bash
bash scripts/build-linux.sh
bash scripts/package-linux.sh
```

可设置 `FV_BUILD_DIR`、`ONNXRUNTIME_ROOT`；通用 CMake 参数为 `-DONNXRUNTIME_ROOT=SDK绝对路径 -DCMAKE_PREFIX_PATH=Qt路径`。

CTest 覆盖核心服务、桌面启动、角色入口、模拟采集、录像时间轴、开关分段、连续采集、掉线重连、长流归档、光照扰动与取消归档。旧训练基线仅编入测试程序，不链接产品。开发命令：`FaceVitals --smoke --screenshot 界面.png`、`FaceVitals --demo-report 输出目录`。

详见 `更新说明_0.6.md` 与 `DEPENDENCIES.md`。旧版说明和分发包保留供追溯；当前行为以此文档为准。

## 开源许可

本项目代码采用 MIT 许可，见 [LICENSE](LICENSE)。Qt、ONNX Runtime、预训练权重和测试图片各自遵循上游许可，见 [THIRD_PARTY.md](THIRD_PARTY.md)。仓库不包含用户账号数据库、人脸录像、实测逐帧数据或研究数据集视频。
