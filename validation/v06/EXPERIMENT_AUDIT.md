# FaceVitals 0.6 实验与发布审计

日期：2026-10-17。审计方式：fresh same-family Codex agent，read-only；acceptance_status=provisional。

**Overall verdict: WARN；integrity_status=warn。**

审计未发现伪造参考值、自归一化准确率、虚构数值结果、公开数据隐私泄漏或本次检查范围内阻止发布的代码缺陷。持续心率可用性、真人心率准确度和多人表情准确度验收仍未完成。

| 检查 | 结论 | 依据 |
|---|---|---|
| A 参考来源 | PASS | tests/TestPulseRobustness.cpp 的固定 BPM/种子模拟；tests/ReplayCheck.cpp:53 的 CSV HR；硬件没有同步参考和表情标注。 |
| B 分数归一化 | PASS | 波形标准化、频谱集中度和 softmax 均未作为准确率；模型分数未经校准。 |
| C 数值与文件 | PASS | validation/v06/pulse-tests.txt、emotion-tests.txt、hardware-summary.json、replay-summary.json、windows-ctest.txt 均存在，数量与记录一致。 |
| D 实际调用 | PASS | CaptureWorker 调用当前 Tracker/Analyzer；ReplayCheck 调用当前与冻结分析器；CMake 注册五组测试。 |
| E 范围 | WARN | 单台 RGB 摄像头、四次 70 秒运行；一张图片的 22 个变体；一个同步未确认的 120 秒回放；没有独立人群准确度。 |
| F 验证类型 | PASS | 脉搏 simulation_only；图片 synthetic_proxy；未标注摄像头仅可用性检查；回放参考源 real_gt 但未确认同步、无可评分窗口。 |

关键证据：取样框修正后运行 69 次分析中，20 次积累、41 次低质量、8 次有效；这发生于最终带通滤波加入前。最终版本实机运行大部分时间未检测到人脸，因此不能宣布稳定显示问题已解决。模拟 200 例通过和噪声 0/400 输出均不是人体准确率。回放两版均无有效输出，不能报告 MAE 改善。

血压和 SpO2 在结果、界面与报告保持 unsupported；没有用心率或正常值代填。公开汇总不含个人心率值、人脸几何、逐帧信号、账号数据库或视频。

## 发布后的执行方核验

审计输出时 GitHub 附件上传仍待完成。随后执行方已完成：公开仓库与预发布可访问；GitHub 三个附件摘要匹配本地；通过匿名公开链接下载 Windows ZIP；ZIP SHA-256 与全部 78 项 payload 哈希匹配；该下载包解压后 smoke 启动退出码 0。详见 release-receipt.json。

这些发布核验由执行方完成，不提高 same-family/provisional 审计的独立性，也不增加生理准确度证据。发布源码与二进制固定于 v0.6.0 / de5c76b；本审计作为后续文档记录。

## 后续验收

1. 在完整人脸、稳定照明下重复最终版本实测，并使用同步、经过验证的脉搏参考设备，报告覆盖率、拒绝原因与误差。
2. 建立独立标注的多人表情集，报告每类指标、混淆矩阵和拒绝率。
3. MPU 文件配对与同步偏移权威确认前，保持 alignment_status=unconfirmed，不用最佳偏移搜索制造准确度。
