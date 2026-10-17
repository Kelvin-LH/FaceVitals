#include "CaptureWorker.h"
#include "ICameraSource.h"
#include "VideoRecorder.h"
#include "SessionArchive.h"
#include "algorithms/EmotionModel.h"
#include "algorithms/EmotionSmoother.h"
#include "algorithms/SignalAnalyzer.h"
#include "algorithms/FaceSignalTracker.h"
#include "algorithms/RoiSignal.h"
#include "algorithms/PulseDisplay.h"
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QPainter>
#include <cmath>

namespace fv {
CaptureWorker::CaptureWorker(Measurement m, QString c, QString d, QString model, int duration,
                             QObject *parent)
    : QThread(parent), measurement_(std::move(m)), camera_(std::move(c)), detector_(std::move(d)),
      model_(std::move(model)), duration_(duration) {}
Preview CaptureWorker::latest() {
    QMutexLocker guard(&mutex_);
    return preview_;
}
void CaptureWorker::run() {
    SignalAnalyzer analyzer;
    EmotionSmoother smoother;
    VideoRecorder recorder;
    SessionArchive archive;
    QString videoName;
    double videoStart = 0;
    int segment = 0;
    auto finishVideo = [&] {
        if (!recorder.isOpen()) return;
        recorder.close();
        measurement_.recordings.append(QJsonObject{{"file", videoName}, {"start_seconds", videoStart},
            {"frames", recorder.frames()}, {"fps", VideoRecorder::fps},
            {"sha256", fileHash(recordingDirectory_ + "/" + videoName)}});
    };
    try {
        archive.open(archiveDirectory_);
        std::unique_ptr<ICameraSource> camera;
        std::unique_ptr<IFaceDetector> detector;
        std::unique_ptr<IEmotionModel> emotion;
        if (!measurement_.synthetic) {
            detector = makeFaceDetector(detector_);
            emotion = makeEmotionModel(model_);
            if ((!detector || !emotion) && !cameraFactory_) fail("识别模型未就绪，无法开始采集。");
        }
        QElapsedTimer timer;
        timer.start();
        double lastFace = -1, lastPulse = -1, lastFrame = 0;
        QRect face;
        FaceDetection detected;
        EmotionResult er;
        PulseResult pulse, lastGoodPulse;
        double retryAt = 0;
        FaceSignalTracker tracker;
        QString cameraMode;
        while (!cancelled_) {
            double t = timer.nsecsElapsed() / 1e9;
            if (duration_ > 0 && t >= duration_)
                break;
            QImage frame;
            if (measurement_.synthetic) {
                frame = QImage(640, 480, QImage::Format_RGB888);
                frame.fill(QColor(22, 40, 55));
                QThread::msleep(33);
                t = timer.nsecsElapsed() / 1e9;
            } else {
                try {
                    if (!camera) {
                        if (t < retryAt) { QThread::msleep(50); continue; }
                        camera = cameraFactory_ ? cameraFactory_() : createCamera(cameraOptions_);
                        camera->open(camera_);
                        cameraMode = camera->description();
                        lastFrame = t;
                    }
                    frame = camera->read();
                    t = timer.nsecsElapsed() / 1e9;
                    if (frame.isNull() && t - lastFrame > 5) fail("5 秒未收到视频帧");
                } catch (const std::exception &e) {
                    if (duration_ > 0) throw;
                    camera.reset(); retryAt = t + 2;
                    finishVideo();
                    Sample gap; gap.t = t; gap.breakContinuity = true; gap.qualityReason = "camera_disconnected"; archive.sample(measurement_, gap);
                    face = {}; lastFace = -1;
                    tracker.reset(); smoother.reset(); er = {}; pulse = {}; lastGoodPulse = {};
                    QMutexLocker guard(&mutex_);
                    preview_ = {};
                    preview_.elapsed = t;
                    preview_.guidance = "摄像头暂不可用，正在自动重连 · " + QString::fromUtf8(e.what());
                    continue;
                }
            }
            if (frame.isNull()) {
                continue;
            }
            lastFrame = t;
            if (measurement_.synthetic) {
                QPainter painter(&frame);
                painter.setPen(Qt::white);
                painter.drawText(QRect(16, 16, 500, 30), Qt::AlignLeft, QString("SIMULATION  %1 s").arg(t, 0, 'f', 1));
            }
            try {
                if (recording_ && !recordingDirectory_.isEmpty()) {
                    if (recorder.isOpen() && (t - videoStart >= segmentSeconds_ || recorder.bytesWritten() >= 512LL * 1024 * 1024))
                        finishVideo();
                    if (!recorder.isOpen()) {
                        videoName = QString("clip-%1.avi").arg(++segment, 3, 10, QChar('0'));
                        videoStart = t;
                        recorder.open(recordingDirectory_ + "/" + videoName, frame, t);
                    } else recorder.append(frame, t);
                } else finishVideo();
            } catch (const std::exception &e) {
                recording_ = false;
                measurement_.recordingError = QString::fromUtf8(e.what());
                try { finishVideo(); } catch (...) { }
            }
            Sample sample;
            sample.t = t;
            QString hint;
            if (measurement_.synthetic) {
                double a = std::sin(2 * 3.141592653589793 * 1.2 * t);
                sample.r = 120 + .2 * a;
                sample.g = 100 + .8 * a;
                sample.b = 80 + .1 * a;
                sample.valid = true; sample.qualityReason = "valid";
                hint = "模拟信号 · 72 bpm · 仅测试软件链路";
                er.status = "synthetic";
                er.label = "模拟数据不生成表情结论";
            } else {
                if (detector && t - lastFace >= .2) {
                    lastFace = t;
                    auto faces = detector->detectDetailed(frame);
                    tracker.detected(faces,t);
                    detected=tracker.detection; face=detected.box;
                    measurement_.diagnostics["last_face_geometry"]=QJsonObject{{"x",face.x()},{"y",face.y()},
                        {"width",face.width()},{"height",face.height()},{"visible_fraction",detected.visibleFraction}};
                    const auto crop = emotionFaceCrop(frame,detected);
                    if (emotion && face.width() >= 80 && !crop.isNull()) {
                        er = emotion->predict(crop); er.t=t; er=smoother.update(er);
                        archive.emotion(measurement_,er);
                    } else { smoother.reset(); er={}; er.status=face.isEmpty()?"no_face":"poor_image"; }
                }
                sample=tracker.sample(frame,t).sample;
                hint=sample.valid?"正在采集面部颜色信号":sample.qualityReason=="no_face"?"未检测到人脸，请正对镜头":
                    sample.qualityReason=="multiple_faces"?"检测到多人，请保持单人测量":
                    sample.qualityReason=="face_too_small_or_clipped"?"请让完整人脸进入画面，并适当靠近镜头":
                    "请保持面部稳定，避免遮挡和过曝";
            }

            archive.sample(measurement_, sample);
            if(sample.breakContinuity) { pulse={}; lastGoodPulse={}; }
            if (!sample.valid) { pulse.heartRate={}; pulse.heartRate.status="temporarily_unavailable"; }
            if (t - lastPulse >= 1) {
                lastPulse = t;
                pulse = analyzer.analyze(measurement_.samples);
                if(pulse.heartRate.status=="valid") {
                    lastGoodPulse=pulse;
                    measurement_.diagnostics["valid_heart_rate_updates"] = measurement_.diagnostics["valid_heart_rate_updates"].toInteger()+1;
                    measurement_.diagnostics["last_valid_heart_rate"] = QJsonObject{{"bpm",pulse.heartRate.value},{"at_seconds",pulse.measuredAt}};
                }
                auto statuses=measurement_.diagnostics["analysis_status_counts"].toObject();
                statuses[pulse.heartRate.status]=statuses[pulse.heartRate.status].toInteger()+1;
                measurement_.diagnostics["analysis_status_counts"]=statuses;
                auto reasons=measurement_.diagnostics["analysis_reason_counts"].toObject();
                reasons[pulse.heartRate.reason]=reasons[pulse.heartRate.reason].toInteger()+1;
                measurement_.diagnostics["analysis_reason_counts"]=reasons;
                if(pulse.heartRate.status=="valid" && !measurement_.diagnostics.contains("first_valid_at_seconds"))
                    measurement_.diagnostics["first_valid_at_seconds"]=pulse.measuredAt;
            }
            {
                QMutexLocker guard(&mutex_);
                if (!measurement_.recordingError.isEmpty()) hint = measurement_.recordingError;
                else if (recorder.isOpen()) hint = "● 录像中 · " + hint;
                else if(sample.valid && pulse.heartRate.status=="low_quality") hint=pulse.heartRate.reason;
                if (!cameraMode.isEmpty()) hint += " · " + cameraMode;
                preview_ = {frame, face, hint, t, er, pulseForDisplay(pulse,lastGoodPulse,t), {}, {}};
                preview_.samples = measurement_.samples.mid(std::max(0, int(measurement_.samples.size()) - 1800));
                preview_.recordingError = measurement_.recordingError;
            }
        }
        measurement_.pulse = analyzer.analyze(measurement_.samples);
        measurement_.status = cancelled_ && duration_ > 0                       ? "cancelled"
                              : measurement_.pulse.heartRate.status == "valid" ? "completed"
                                                                               : "partial";
        measurement_.reason =
            cancelled_ ? "用户停止采集。" : measurement_.pulse.heartRate.reason;
    } catch (const std::exception &e) {
        measurement_.status = "failed";
        measurement_.reason = QString::fromUtf8(e.what());
        measurement_.pulse = analyzer.analyze(measurement_.samples);
    }
    measurement_.ended = utcNow();
    try { archive.close(); } catch (const std::exception &e) { measurement_.status = "failed"; measurement_.reason = QString::fromUtf8(e.what()); }
    try { finishVideo(); } catch (const std::exception &e) { measurement_.recordingError = QString::fromUtf8(e.what()); }
    emit completed(measurement_);
}
} // namespace fv
