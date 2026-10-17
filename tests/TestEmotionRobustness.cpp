#include "algorithms/EmotionModel.h"
#include "algorithms/EmotionSmoother.h"
#include <QPainter>
#include <QTest>
#include <algorithm>
#include <cmath>

using namespace fv;

namespace {
double overlap(const QRectF &a, const QRectF &b) {
    const auto intersection = a.intersected(b);
    const double area = intersection.width() * intersection.height();
    return area / (a.width() * a.height() + b.width() * b.height() - area);
}
QImage exposure(QImage image, double factor) {
    image = image.convertToFormat(QImage::Format_RGB888);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width() * 3; ++x)
            image.scanLine(y)[x] = uchar(std::clamp(int(std::lround(image.constScanLine(y)[x] * factor)), 0, 255));
    return image;
}
}

class TestEmotionRobustness : public QObject {
    Q_OBJECT
  private slots:
    void aspectTranslationAndLight();
    void tiltedFace();
    void poorImagesAndEmptyFrame();
    void cropBoundaries();
    void smootherInvalidOutputAndRecovery();
};

void TestEmotionRobustness::tiltedFace() {
#ifdef FV_HAS_ONNX
    const auto root=QString::fromUtf8(FV_SOURCE_DIR);
    auto detector=makeFaceDetector(root+"/models/bootstrap/face.onnx");
    auto model=makeEmotionModel(root+"/models/bootstrap");
    const QImage source(root+"/tests/fixtures/astronaut.png");
    for(int degrees : {-20,-10,10,20}) {
        QImage frame(720,720,QImage::Format_RGB888);frame.fill(Qt::black);
        QPainter p(&frame);p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.translate(360,360);p.rotate(degrees);p.drawImage(QPoint(-256,-256),source);p.end();
        const auto faces=detector->detectDetailed(frame);QCOMPARE(faces.size(),1);
        const auto crop=emotionFaceCrop(frame,faces.first());QVERIFY(!crop.isNull());
        const auto result=model->predict(crop);
        QCOMPARE(result.status,QString("valid"));QCOMPARE(result.label,QString::fromUtf8("高兴"));
    }
#else
    QSKIP("ONNX optional backend not built");
#endif
}

void TestEmotionRobustness::aspectTranslationAndLight() {
#ifdef FV_HAS_ONNX
    const auto root = QString::fromUtf8(FV_SOURCE_DIR);
    auto detector = makeFaceDetector(root + "/models/bootstrap/face.onnx");
    auto model = makeEmotionModel(root + "/models/bootstrap");
    const QImage source(root + "/tests/fixtures/astronaut.png");
    QVERIFY(!source.isNull());
    const auto reference = detector->detectDetailed(source);
    QCOMPARE(reference.size(), 1);
    struct Scene { int w, h, x, y; double scale; };
    const QVector<Scene> scenes{{640, 640, 40, 40, 1}, {960, 540, 180, 20, .9},
        {1280, 720, 280, 30, 1.2}, {540, 960, 30, 180, .9},
        {480, 854, 20, 180, .8}, {1024, 576, 430, 40, .8}};
    int cases = 0;
    double minimumOverlap = 1, minimumScore = 1;
    // 同一公有领域样例的 18 个输入变体，仅验证几何/输出回归，不代表人群准确率。
    for (const auto &scene : scenes) {
        for (double light : {.65, 1., 1.25}) {
            QImage frame(scene.w, scene.h, QImage::Format_RGB888);
            frame.fill(Qt::black);
            const int side = int(std::lround(source.width() * scene.scale));
            const auto scaled = exposure(source.scaled(side, side, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), light);
            { QPainter painter(&frame); painter.drawImage(scene.x, scene.y, scaled); }
            const auto faces = detector->detectDetailed(frame);
            QCOMPARE(faces.size(), 1);
            const auto &face = faces.first();
            QCOMPARE(face.landmarks.size(), 5);
            QVERIFY(face.confidence >= .8);
            QVERIFY(face.visibleFraction > .99);
            const auto ref = reference.first().unclippedBox;
            const double scale = side / double(source.width());
            const QRectF expected(ref.x() * scale + scene.x, ref.y() * scale + scene.y,
                                  ref.width() * scale, ref.height() * scale);
            const double iou = overlap(face.unclippedBox, expected);
            minimumOverlap = std::min(minimumOverlap, iou);
            QVERIFY2(iou > .7, qPrintable(QString("%1x%2 light=%3 IoU=%4").arg(scene.w).arg(scene.h).arg(light).arg(iou)));
            for (int i = 0; i < 5; ++i) {
                const QPointF expectedPoint = reference.first().landmarks[i] * scale + QPointF(scene.x, scene.y);
                const auto delta = face.landmarks[i] - expectedPoint;
                QVERIFY(std::hypot(delta.x(), delta.y()) < expected.width() * .18);
            }
            const auto crop = emotionFaceCrop(frame, face);
            QVERIFY(!crop.isNull());
            const auto result = model->predict(crop);
            QCOMPARE(result.status, QString("valid"));
            QCOMPARE(result.label, QString::fromUtf8("高兴"));
            minimumScore = std::min(minimumScore, *std::max_element(result.scores.begin(), result.scores.end()));
            ++cases;
        }
    }
    qInfo().noquote() << QJsonDocument(QJsonObject{{"transformation_cases", cases},
        {"minimum_box_consistency_iou", minimumOverlap}, {"minimum_happy_score", minimumScore},
        {"scope", "single-public-image regression; not population accuracy"}}).toJson(QJsonDocument::Compact);
#else
    QSKIP("ONNX optional backend not built");
#endif
}

void TestEmotionRobustness::poorImagesAndEmptyFrame() {
#ifdef FV_HAS_ONNX
    const auto root = QString::fromUtf8(FV_SOURCE_DIR);
    auto detector = makeFaceDetector(root + "/models/bootstrap/face.onnx");
    QVERIFY(detector->detect({}).isEmpty());
    QVERIFY(detector->detectDetailed({}).isEmpty());
    auto model = makeEmotionModel(root + "/models/bootstrap");
    for (int level : {0, 10, 128, 245, 255}) {
        QImage image(224, 224, QImage::Format_RGB888);
        image.fill(QColor(level, level, level));
        auto result = model->predict(image);
        QCOMPARE(result.status, QString("poor_image"));
        QCOMPARE(result.label, QString::fromUtf8("无法判断"));
        QCOMPARE(result.scores.size(), 8);
        EmotionSmoother smoother;
        QCOMPARE(smoother.update(result).status, QString("poor_image"));
    }
#else
    QSKIP("ONNX optional backend not built");
#endif
}

void TestEmotionRobustness::cropBoundaries() {
    QImage image(400, 300, QImage::Format_RGB888);
    image.fill(Qt::gray);
    FaceDetection face;
    face.box = QRect(50, 50, 100, 120);
    face.unclippedBox = face.box;
    QVERIFY(!emotionFaceCrop(image, face).isNull());
    face.visibleFraction = .6;
    QVERIFY(emotionFaceCrop(image, face).isNull());
    face.visibleFraction = 1;
    face.unclippedBox = QRectF(-50, 50, 100, 120);
    face.box = QRect(0, 50, 50, 120);
    QVERIFY(emotionFaceCrop(image, face).isNull());
    face.box = QRect(50, 50, 100, 120);
    face.unclippedBox = face.box;
    face.landmarks = {QPointF(-1, 80)};
    QVERIFY(emotionFaceCrop(image, face).isNull());
}

void TestEmotionRobustness::smootherInvalidOutputAndRecovery() {
    EmotionSmoother smoother;
    EmotionResult r;
    r.labels = {"a", "b"}; r.scores = {.95, .05}; r.t = 0;
    QCOMPARE(smoother.update(r).label, QString("a"));
    r.status = "poor_image"; r.label = "无法判断"; r.t = .2;
    QCOMPARE(smoother.update(r).status, QString("poor_image"));
    r.status = "valid"; r.scores = {.05, .95}; r.t = .4;
    QCOMPARE(smoother.update(r).label, QString("b"));
    r.scores = {std::numeric_limits<double>::quiet_NaN(), .95}; r.t = .6;
    QCOMPARE(smoother.update(r).status, QString("invalid_output"));
    r.scores = {.9}; r.t = .8;
    QCOMPARE(smoother.update(r).status, QString("invalid_output"));
    r.scores = {0., 0.}; r.t = 1.;
    QCOMPARE(smoother.update(r).status, QString("invalid_output"));
    r.scores = {.9, .1}; r.t = 1.2;
    QCOMPARE(smoother.update(r).label, QString("a"));
    r.scores = {.1, .9}; r.t = 3.;
    QCOMPARE(smoother.update(r).label, QString("b"));
}

QTEST_GUILESS_MAIN(TestEmotionRobustness)
#include "TestEmotionRobustness.moc"
