#pragma once
#include "domain/Types.h"
#include <memory>

namespace fv {
class IEmotionModel {
  public:
    virtual ~IEmotionModel() = default;
    virtual EmotionResult predict(const QImage &face) = 0;
};
struct FaceDetection {
    QRect box;
    QRectF unclippedBox;
    // 与 YuNet 官方顺序一致：右眼、左眼、鼻尖、右嘴角、左嘴角。
    QVector<QPointF> landmarks;
    double confidence = 0;
    double visibleFraction = 1;
};
class IFaceDetector {
  public:
    virtual ~IFaceDetector() = default;
    virtual QVector<QRect> detect(const QImage &frame) = 0;
    virtual QVector<FaceDetection> detectDetailed(const QImage &frame);
};
QImage emotionFaceCrop(const QImage &frame, const FaceDetection &face);
class LinearEmotionModel final : public IEmotionModel {
  public:
    explicit LinearEmotionModel(const QJsonObject &model);
    EmotionResult predict(const QImage &face) override;
    static QVector<double> features(const QImage &face);
    static QVector<double> probabilities(const QVector<double> &f, const QVector<QVector<double>> &weights);
    static constexpr int dimensions = 128;

  private:
    QStringList labels_;
    QVector<QVector<double>> weights_;
};
std::unique_ptr<IEmotionModel> makeEmotionModel(const QString &directory);
std::unique_ptr<IFaceDetector> makeFaceDetector(const QString &path);
#ifdef FV_HAS_ONNX
std::unique_ptr<IEmotionModel> makeOnnxEmotion(const QString &path, const QJsonObject &manifest);
std::unique_ptr<IFaceDetector> makeOnnxFaceDetector(const QString &path);
#endif
} // namespace fv
