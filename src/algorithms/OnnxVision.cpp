#include "EmotionModel.h"
#define ORT_API_MANUAL_INIT
#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <onnxruntime_cxx_api.h>

namespace fv {
namespace {
class OrtModel {
  public:
    explicit OrtModel(const QString &path) : env_(nullptr), session_(nullptr) {
        static std::once_flag initialized;
        std::call_once(initialized, [] {
            const auto *api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
            if (!api)
                fail("ONNX Runtime DLL 版本不匹配，请使用本程序附带的运行库。");
            Ort::InitApi(api);
        });
        env_ = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "FaceVitals");
        auto bytes = readFile(path, 64 * 1024 * 1024);
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(2);
        options.SetInterOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
        // 从字节加载，不允许模型包引用外部数据路径，也不注册自定义算子库。
        session_ = Ort::Session(env_, bytes.constData(), size_t(bytes.size()), options);
        if (session_.GetInputCount() != 1)
            fail("模型必须只有一个图像输入。");
        auto inputType = session_.GetInputTypeInfo(0);
        auto info = inputType.GetTensorTypeAndShapeInfo();
        if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            fail("模型输入必须为 float32。");
        Ort::AllocatorWithDefaultOptions allocator;
        input_ = session_.GetInputNameAllocated(0, allocator).get();
    }
    std::vector<Ort::Value> run(std::vector<float> &input, int size,
                                const std::vector<const char *> &outputs) {
        std::array<int64_t, 4> shape{1, 3, size, size};
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), 4);
        const char *name = input_.c_str();
        return session_.Run(Ort::RunOptions{nullptr}, &name, &tensor, 1, outputs.data(), outputs.size());
    }
    Ort::Session &session() {
        return session_;
    }

  private:
    Ort::Env env_;
    Ort::Session session_;
    std::string input_;
};
class OnnxEmotion final : public IEmotionModel {
  public:
    OnnxEmotion(const QString &path, const QJsonObject &manifest) : model_(path) {
        for (auto x : manifest["labels"].toArray())
            labels_.append(x.toString());
        if (labels_.size() < 2 || labels_.size() > 16)
            fail("类别数量无效。");
        if (model_.session().GetOutputCount() != 1)
            fail("表情模型需要单输出。");
        auto inputType = model_.session().GetInputTypeInfo(0);
        auto info = inputType.GetTensorTypeAndShapeInfo();
        auto shape = info.GetShape();
        if (shape.size() != 4 || shape[1] != 3 || shape[2] != 224 || shape[3] != 224)
            fail("模型张量与清单中的 NCHW RGB 尺寸不匹配。");
        Ort::AllocatorWithDefaultOptions a;
        output_ = model_.session().GetOutputNameAllocated(0, a).get();
    }
    EmotionResult predict(const QImage &image) override {
        auto img = image.convertToFormat(QImage::Format_RGB888)
                       .scaled(224, 224, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (img.isNull())
            fail("人脸图像无效。");
        std::vector<float> input(3 * 224 * 224);
        const float mean[] = {.485f, .456f, .406f}, sd[] = {.229f, .224f, .225f};
        double luminance = 0, luminanceSquared = 0;
        int clipped = 0;
        for (int y = 0; y < 224; ++y)
            for (int x = 0; x < 224; ++x) {
                const auto *pixel = img.constScanLine(y) + x * 3;
                const double value = .2126 * pixel[0] + .7152 * pixel[1] + .0722 * pixel[2];
                luminance += value;
                luminanceSquared += value * value;
                if (value < 8 || value > 247) ++clipped;
                for (int c = 0; c < 3; ++c)
                    input[c * 224 * 224 + y * 224 + x] =
                        (pixel[c] / 255.f - mean[c]) / sd[c];
            }
        auto result = model_.run(input, 224, {output_.c_str()});
        auto info = result[0].GetTensorTypeAndShapeInfo();
        if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            info.GetElementCount() != size_t(labels_.size()))
            fail("模型输出契约无效。");
        auto p = result[0].GetTensorData<float>();
        EmotionResult r;
        r.labels = labels_;
        double mx = *std::max_element(p, p + labels_.size()), total = 0;
        for (int i = 0; i < labels_.size(); ++i) {
            if (!std::isfinite(p[i]))
                fail("模型产生无效分数。");
            double v = std::exp(p[i] - mx);
            r.scores.append(v);
            total += v;
        }
        for (double &v : r.scores)
            v /= total;
        auto best = std::max_element(r.scores.begin(), r.scores.end());
        r.status = *best >= .5 ? "valid" : "uncertain";
        if (r.status == "valid")
            r.label = labels_[std::distance(r.scores.begin(), best)];
        // 严重曝光异常或无纹理图像仍可能产生高 softmax 分数；它不是可靠表情证据。
        constexpr double pixels = 224.0 * 224;
        const double average = luminance / pixels;
        const double contrast = std::sqrt(std::max(0., luminanceSquared / pixels - average * average));
        if (average < 20 || average > 240 || contrast < 7 || clipped / pixels > .65) {
            r.status = "poor_image";
            r.label = "无法判断";
        }
        return r;
    }

  private:
    OrtModel model_;
    QStringList labels_;
    std::string output_;
};
class YuNetDetector final : public IFaceDetector {
  public:
    explicit YuNetDetector(const QString &path) : model_(path) {}
    QVector<QRect> detect(const QImage &image) override {
        QVector<QRect> result;
        for (const auto &face : detectDetailed(image)) result.append(face.box);
        return result;
    }
    QVector<FaceDetection> detectDetailed(const QImage &image) override {
        if (image.isNull()) return {};
        // YuNet 模型张量为 BGR、原始 0—255，和表情模型的预处理严格分开。
        // 保留人脸几何比例并右/下补零，避免 16:9、竖屏摄像头被强制拉伸。
        constexpr int size = 640;
        auto img = image.convertToFormat(QImage::Format_RGB888)
                       .scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        std::vector<float> input(3 * size * size, 0.f);
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
                for (int c = 0; c < 3; ++c)
                    input[c * size * size + y * size + x] = img.constScanLine(y)[x * 3 + 2 - c];
        std::vector<const char *> names{"cls_8",  "cls_16", "cls_32",  "obj_8",  "obj_16",
                                        "obj_32", "bbox_8", "bbox_16", "bbox_32",
                                        "kps_8", "kps_16", "kps_32"};
        auto out = model_.run(input, size, names);
        QVector<FaceDetection> candidates;
        const double sx = image.width() / double(img.width()), sy = image.height() / double(img.height());
        for (int level = 0; level < 3; ++level) {
            int stride = 8 << level, grid = size / stride;
            for (int group = 0; group < 4; ++group) {
                auto type = out[level + group * 3].GetTensorTypeAndShapeInfo();
                if (type.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                    type.GetElementCount() != size_t(grid * grid * (group == 3 ? 10 : group == 2 ? 4 : 1)))
                    fail("人脸模型输出形状不匹配。");
            }
            const auto *cls = out[level].GetTensorData<float>(), *obj = out[level + 3].GetTensorData<float>(),
                       *box = out[level + 6].GetTensorData<float>(), *kps = out[level + 9].GetTensorData<float>();
            for (int k = 0; k < grid * grid; ++k) {
                if (!std::isfinite(cls[k]) || !std::isfinite(obj[k])) continue;
                double score = std::sqrt(std::clamp(cls[k], 0.f, 1.f) * std::clamp(obj[k], 0.f, 1.f));
                if (score < .8)
                    continue;
                bool finite = true;
                for (int n = 0; n < 4; ++n) finite &= std::isfinite(box[k * 4 + n]);
                for (int n = 0; n < 10; ++n) finite &= std::isfinite(kps[k * 10 + n]);
                if (!finite) continue;
                double cx = (k % grid + box[k * 4]) * stride, cy = (k / grid + box[k * 4 + 1]) * stride;
                double w = std::exp(std::clamp(box[k * 4 + 2], -10.f, 10.f)) * stride,
                       h = std::exp(std::clamp(box[k * 4 + 3], -10.f, 10.f)) * stride;
                FaceDetection face;
                face.unclippedBox = QRectF((cx - w / 2) * sx, (cy - h / 2) * sy, w * sx, h * sy);
                const auto visible = face.unclippedBox.intersected(QRectF(image.rect()));
                if (visible.isEmpty()) continue;
                face.box = visible.toAlignedRect().intersected(image.rect());
                face.confidence = score;
                face.visibleFraction = visible.width() * visible.height() /
                                       (face.unclippedBox.width() * face.unclippedBox.height());
                for (int n = 0; n < 5; ++n)
                    face.landmarks.append(QPointF((k % grid + kps[k * 10 + 2 * n]) * stride * sx,
                                                  (k / grid + kps[k * 10 + 2 * n + 1]) * stride * sy));
                if (face.box.width() > 35 && face.box.height() > 35)
                    candidates.append(face);
            }
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto &a, const auto &b) { return a.confidence > b.confidence; });
        QVector<FaceDetection> faces;
        for (const auto &candidate : candidates) {
            bool keep = true;
            for (const auto &face : faces) {
                const auto intersection = candidate.unclippedBox.intersected(face.unclippedBox);
                double a = intersection.width() * intersection.height();
                double u = face.unclippedBox.width() * face.unclippedBox.height() +
                           candidate.unclippedBox.width() * candidate.unclippedBox.height() - a;
                if (u > 0 && a / u > .3) {
                    keep = false;
                    break;
                }
            }
            if (keep)
                faces.append(candidate);
            if (faces.size() >= 10)
                break;
        }
        return faces;
    }

  private:
    OrtModel model_;
};
} // namespace
std::unique_ptr<IEmotionModel> makeOnnxEmotion(const QString &p, const QJsonObject &m) {
    return std::make_unique<OnnxEmotion>(p, m);
}
std::unique_ptr<IFaceDetector> makeOnnxFaceDetector(const QString &p) {
    return std::make_unique<YuNetDetector>(p);
}
} // namespace fv
