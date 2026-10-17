#include "EmotionModel.h"
#include <algorithm>
#include <cmath>
#include <QPainter>

namespace fv {
QVector<FaceDetection> IFaceDetector::detectDetailed(const QImage &frame) {
    QVector<FaceDetection> result;
    for (const auto &box : detect(frame)) {
        FaceDetection face;
        face.box = box;
        face.unclippedBox = box;
        result.append(face);
    }
    return result;
}
QImage emotionFaceCrop(const QImage &frame, const FaceDetection &face) {
    // 截断的脸不能通过再次拉伸补回缺失的眼睛/嘴部；等待完整人脸回到画面。
    if (frame.isNull() || face.box.isEmpty() || !std::isfinite(face.visibleFraction) ||
        face.visibleFraction < .9)
        return {};
    if (!face.unclippedBox.isEmpty()) {
        const auto visible = face.unclippedBox.intersected(QRectF(frame.rect()));
        const double area = face.unclippedBox.width() * face.unclippedBox.height();
        if (!std::isfinite(area) || visible.width() * visible.height() < .9 * area)
            return {};
    }
    for (const auto &point : face.landmarks)
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
            !QRectF(frame.rect()).contains(point))
            return {};
    auto crop=frame.copy(face.box.intersected(frame.rect()));
    if(face.landmarks.size()==5) {
        auto left=face.landmarks[0],right=face.landmarks[1];
        if(left.x()>right.x()) std::swap(left,right);
        const double angle=std::atan2(right.y()-left.y(),right.x()-left.x())*180/3.141592653589793;
        if(std::abs(angle)>=3 && std::abs(angle)<=30 && right.x()-left.x()>face.box.width()*.15) {
            QTransform transform;
            const auto center=QRectF(face.box).center();
            transform.translate(crop.width()/2.,crop.height()/2.);
            transform.rotate(-angle); transform.translate(-center.x(),-center.y());
            bool ok=false;const auto inverse=transform.inverted(&ok);
            if(ok && QRectF(frame.rect()).contains(inverse.mapRect(QRectF(crop.rect())))) {
                QImage aligned(crop.size(),QImage::Format_RGB888);aligned.fill(Qt::black);
                QPainter p(&aligned);p.setRenderHint(QPainter::SmoothPixmapTransform);p.setTransform(transform);
                p.drawImage(QPoint(0,0),frame);p.end();crop=aligned;
            }
        }
    }
    return crop;
}
QVector<double> LinearEmotionModel::features(const QImage &face) {
    if (face.isNull())
        fail("人脸图像为空。");
    auto img = face.convertToFormat(QImage::Format_Grayscale8)
                   .scaled(32, 32, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QVector<double> f(dimensions, 0);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            int cell = (y / 4) * 8 + x / 4;
            f[cell] += img.constScanLine(y)[x] / 255.0 / 16;
            int dx = int(img.constScanLine(y)[std::min(31, x + 1)]) -
                     int(img.constScanLine(y)[std::max(0, x - 1)]);
            int dy = int(img.constScanLine(std::min(31, y + 1))[x]) -
                     int(img.constScanLine(std::max(0, y - 1))[x]);
            f[64 + cell] += std::sqrt(double(dx * dx + dy * dy)) / 255.0 / 16;
        }
    double mean = 0;
    for (int i = 0; i < 64; ++i)
        mean += f[i] / 64;
    for (int i = 0; i < 64; ++i)
        f[i] -= mean;
    return f;
}
QVector<double> LinearEmotionModel::probabilities(const QVector<double> &f,
                                                  const QVector<QVector<double>> &w) {
    QVector<double> p;
    double maximum = -1e100;
    for (const auto &row : w) {
        double a = row.last();
        for (int j = 0; j < f.size(); ++j)
            a += row[j] * f[j];
        p.append(a);
        maximum = std::max(maximum, a);
    }
    double total = 0;
    for (double &x : p) {
        x = std::exp(x - maximum);
        total += x;
    }
    for (double &x : p)
        x /= total;
    return p;
}
LinearEmotionModel::LinearEmotionModel(const QJsonObject &model) {
    if (model["features"].toString() != "gray-gradient-128-v1")
        fail("未知的特征提取版本。");
    for (auto x : model["labels"].toArray())
        labels_.append(x.toString());
    if (labels_.size() < 2 || labels_.size() > 16)
        fail("类别数量无效。");
    for (auto v : model["weights"].toArray()) {
        QVector<double> row;
        for (auto x : v.toArray()) {
            double n = x.toDouble(std::numeric_limits<double>::quiet_NaN());
            if (!std::isfinite(n) || std::abs(n) > 1e6)
                fail("权重无效。");
            row.append(n);
        }
        if (row.size() != dimensions + 1)
            fail("权重维度无效。");
        weights_.append(row);
    }
    if (weights_.size() != labels_.size())
        fail("权重和类别数量不匹配。");
}
EmotionResult LinearEmotionModel::predict(const QImage &face) {
    EmotionResult r;
    r.labels = labels_;
    r.scores = probabilities(features(face), weights_);
    auto i = std::max_element(r.scores.begin(), r.scores.end());
    r.status = *i >= .5 ? "valid" : "uncertain";
    if (r.status == "valid")
        r.label = labels_[std::distance(r.scores.begin(), i)];
    return r;
}
std::unique_ptr<IEmotionModel> makeEmotionModel(const QString &dir) {
    if (dir.isEmpty())
        return {};
    auto m = readJson(dir + "/manifest.json");
    auto path = dir + "/" + m["file"].toString();
    if (fileHash(path) != m["sha256"].toString())
        fail("模型哈希不匹配。");
    if (m["format"].toString() == "linear-v1")
        return std::make_unique<LinearEmotionModel>(readJson(path));
#ifdef FV_HAS_ONNX
    if (m["format"].toString() == "onnx-emotion-v1")
        return makeOnnxEmotion(path, m);
#endif
    fail("此构建不支持该模型格式。");
    return {};
}
std::unique_ptr<IFaceDetector> makeFaceDetector(const QString &path) {
#ifdef FV_HAS_ONNX
    if (!path.isEmpty())
        return makeOnnxFaceDetector(path);
#else
    Q_UNUSED(path);
#endif
    return {};
}
} // namespace fv
