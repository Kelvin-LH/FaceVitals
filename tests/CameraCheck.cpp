#include "capture/CaptureWorker.h"
#include "capture/ICameraSource.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTimer>
#ifdef Q_OS_WIN
namespace fv { QVector<CameraInfo> enumerateDirectShowCameras(); }
#endif

// 手动运行的硬件诊断；不加入 CTest，不保存图像、视频或逐帧信号。
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    qRegisterMetaType<fv::Measurement>();
    const auto args = app.arguments();
    if (args.size() < 3) return 2;
    try {
        auto cameras = fv::enumerateCameras();
        if (cameras.isEmpty()) fv::fail("未发现摄像头。");
        if (args.contains("--list")) {
            QJsonArray list;
            for (const auto &c : cameras) list.append(QJsonObject{{"id",c.id},{"name",c.name}});
            fv::writeFile(args[2],QJsonDocument(list).toJson()); return 0;
        }
        fv::Measurement m; m.id = fv::newId(); m.owner = "diagnostic";
        QString device = cameras.first().id;
        QString name = cameras.first().name;
#ifdef Q_OS_WIN
        if (args.contains("--dshow")) for (const auto &c : fv::enumerateDirectShowCameras()) if (c.name == name) { device = c.id; break; }
#endif
        if (args.contains("--obs")) for (const auto &c : cameras) if (c.name == "OBS Virtual Camera") { device = c.id; name = c.name; break; }
        const int seconds=args.contains("--seconds")?args.value(args.indexOf("--seconds")+1).toInt():(args.contains("--long")?45:8);
        if(seconds<1 || seconds>300) fv::fail("诊断时长应为 1—300 秒。");
        fv::CaptureWorker worker(m, device, args[1] + "/face.onnx", args[1], seconds);
        if (args.contains("--compat")) worker.configureCamera({640,480,30});
        QObject::connect(&worker, &fv::CaptureWorker::completed, &app, [&](fv::Measurement result) {
            worker.wait();
            int valid = 0;
            QJsonObject statuses, labels;
            double scoreSum=0;
            for (const auto &e : result.emotions) {
                if (e.status == "valid") ++valid;
                statuses[e.status]=statuses[e.status].toInt()+1;
                if(!e.scores.isEmpty()) {
                    const auto best=std::max_element(e.scores.begin(),e.scores.end());
                    const auto label=e.labels.value(int(best-e.scores.begin()));
                    labels[label]=labels[label].toInt()+1; scoreSum+=*best;
                }
            }
            const auto preview = worker.latest();
            fv::writeFile(args[2], QJsonDocument(QJsonObject{
                {"camera", name}, {"frames", result.samples.size()},
                {"width", preview.image.width()}, {"height", preview.image.height()},
                {"expression_predictions", result.emotions.size()}, {"valid_predictions", valid},
                {"emotion_status_counts",statuses},{"emotion_top_label_counts",labels},
                {"emotion_mean_top_score",result.emotions.isEmpty()?0:scoreSum/result.emotions.size()},
                {"elapsed_seconds",preview.elapsed},{"measured_at_seconds",result.pulse.measuredAt},
                {"capture_failed", result.status == "failed"},
                {"capture_error", result.status == "failed" ? result.reason : QString()},
                {"last_guidance", preview.guidance}, {"recordings", result.recordings.size()}
                ,{"diagnostics",result.diagnostics},{"valid_signal_frames",result.validSamples},{"heart_rate",result.pulse.heartRate.json()}
            }).toJson());
            app.exit(result.status == "failed" || result.samples.isEmpty() ? 1 : 0);
        });
        QTimer::singleShot((seconds+12)*1000, &worker, &fv::CaptureWorker::cancel);
        worker.start();
        return app.exec();
    } catch (const std::exception &e) {
        fv::writeFile(args[2], QJsonDocument(QJsonObject{{"capture_failed", true}, {"error", QString::fromUtf8(e.what())}}).toJson());
        return 1;
    }
}
