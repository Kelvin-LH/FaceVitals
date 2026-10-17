# 初始模型来源

下载于 2026-10-02。仅用于研究原型，未经本项目目标人群验证。

- 表情模型：[EmotiEffLib / enet_b0_8_best_afew.onnx](https://github.com/sb-ai-lab/EmotiEffLib/tree/main/models/affectnet_emotions/onnx)。SHA-256：`7aa2ea31c1311f4f8aa9d3fdb085d418dd4e7a48c4b9ed41df8c044f91d0213f`。输入 RGB，224×224，ImageNet mean/std；8 类顺序取自作者实现。仓库许可证见 EMOTION_LICENSE.txt；训练数据与预训练权重的使用条件需另行确认。
- 人脸模型：[OpenCV Zoo / YuNet 2023mar](https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet)。SHA-256：`8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4`。许可证 MIT，见 FACE_LICENSE.txt。

原生解码代码按照公开模型输出定义实现。未依赖 OpenCV、Python 或作者的 Python/C++ 运行库。
