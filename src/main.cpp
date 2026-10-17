#include "algorithms/SignalAnalyzer.h"
#include "algorithms/EmotionModel.h"
#include "reports/ReportWriter.h"
#include "ui/MainWindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QIcon>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <cmath>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

int main(int argc, char **argv) {
#ifdef Q_OS_WIN
    // 分发版固定使用随包附带的 Windows 插件，避免继承开发环境的 offscreen 或外部 Qt 路径。
    wchar_t executable[32768];
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    if (length > 0 && length < 32768) {
        const auto directory = QFileInfo(QString::fromWCharArray(executable, int(length))).absolutePath();
        if (QFileInfo::exists(directory + "/qt.conf")) {
            qputenv("QT_QPA_PLATFORM", "windows");
            qputenv("QT_PLUGIN_PATH", directory.toUtf8());
            qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", (directory + "/platforms").toUtf8());
        }
    }
#endif
    QApplication app(argc, argv);
    app.setApplicationName("FaceVitals");
    app.setWindowIcon(QIcon(":/brand/facevitals.ico"));
    app.setOrganizationName("FaceVitalsLab");
    app.setApplicationVersion("0.6.0");
    app.setStyle("Fusion");
    QImageReader::setAllocationLimit(128);
    qRegisterMetaType<fv::Measurement>();
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOptions({{{"d", "data-dir"}, "Local data directory", "path"},
                       {{"a", "assets"}, "Bootstrap model directory", "path"},
                       {"smoke", "Offscreen desktop startup check with temporary account"},
                       {"screenshot", "Save smoke screenshot", "path"},
                       {"model-check", "Initialize and verify models; write a diagnostic JSON", "path"},
                       {"check-image", "Optional face image for model-check", "path"},
                       {"demo-report", "Create clearly marked synthetic report", "directory"}});
    parser.process(app);
    try {
        if (parser.isSet("demo-report")) {
            fv::Measurement m;
            m.id = fv::newId();
            m.owner = "synthetic";
            m.subject = "SIMULATION";
            m.created = fv::utcNow();
            m.ended = m.created;
            m.source = "synthetic-72bpm";
            m.synthetic = true;
            m.status = "completed";
            for (int i = 0; i < 1800; ++i) {
                double t = i / 30., a = std::sin(2 * 3.141592653589793 * 1.2 * t);
                m.samples.append({t, 120 + .2 * a, 100 + .8 * a, 80 + .1 * a, true});
            }
            m.pulse = fv::SignalAnalyzer().analyze(m.samples);
            m.reason = "模拟信号用于软件验证，不代表真人测量。";
            QTextStream(stdout) << fv::ReportWriter(parser.value("demo-report")).generate(m) << Qt::endl;
            return 0;
        }
        QTemporaryDir temporary;
        QString root;
        if (parser.isSet("smoke"))
            root = temporary.path();
        else if (parser.isSet("data-dir"))
            root = parser.value("data-dir");
        else
            root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QDir().mkpath(root);
        QLockFile lock(root + "/application.lock");
        lock.setStaleLockTime(0);
        if (!lock.tryLock())
            fv::fail("此数据目录已被另一个程序实例使用。");
        fv::Database db(root);
        fv::AuthService auth(db);
        QString assets = parser.value("assets");
        if (assets.isEmpty())
            assets = QCoreApplication::applicationDirPath() + "/models/bootstrap";
        if (!QFileInfo::exists(assets) && QFileInfo::exists("models/bootstrap"))
            assets = QDir("models/bootstrap").absolutePath();
        fv::ModelRegistry registry(db, auth);
        registry.ensureBundledModel(assets);
        if (fv::fileHash(assets + "/face.onnx") != "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4")
            fv::fail("人脸检测模型校验失败。");
        auto detector = fv::makeFaceDetector(assets + "/face.onnx");
        if (!detector) fv::fail("当前构建不支持人脸识别模型。");
        if (parser.isSet("model-check")) {
            auto emotion = fv::makeEmotionModel(registry.activePath());
            QJsonObject result{{"ready", true}, {"active_model", registry.activeId()}, {"time", fv::utcNow()}};
            if (parser.isSet("check-image")) {
                QImage image(parser.value("check-image"));
                if (image.isNull()) fv::fail("测试图像无法读取。");
                auto faces = detector->detect(image);
                result["face_count"] = faces.size();
                if (faces.size() != 1) fv::fail("测试图像必须检测到一张人脸。");
                result["emotion"] = emotion->predict(image.copy(faces.first())).json();
            }
            fv::writeFile(parser.value("model-check"), QJsonDocument(result).toJson());
            return 0;
        }
        detector.reset();
        QString token;
        if (parser.isSet("smoke")) {
            auth.bootstrap("smoke", "Temporary-test-123!");
            token = auth.login("smoke", "Temporary-test-123!");
        } else
            token = fv::loginDialog(auth);
        if (token.isEmpty())
            return 0;
        fv::MainWindow window(db, auth, token, assets);
        window.show();
        if (parser.isSet("smoke")) {
            QTimer::singleShot(1000, &app, [&] {
                if (parser.isSet("screenshot"))
                    window.grab().save(parser.value("screenshot"));
                app.exit(0);
            });
        }
        return app.exec();
    } catch (const std::exception &e) {
        if (parser.isSet("smoke") || parser.isSet("demo-report") || parser.isSet("model-check")) {
            if (parser.isSet("model-check")) {
                try { fv::writeFile(parser.value("model-check"), QJsonDocument(QJsonObject{{"ready", false}, {"error", QString::fromUtf8(e.what())}}).toJson()); } catch (...) { }
            }
            QTextStream(stderr) << e.what() << Qt::endl;
            return 1;
        }
        QMessageBox::critical(nullptr, "FaceVitals 启动失败", QString::fromUtf8(e.what()));
        return 1;
    }
}
