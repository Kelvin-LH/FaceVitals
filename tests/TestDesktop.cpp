#include "ui/MainWindow.h"
#include "reports/ReportWriter.h"
#include "capture/VideoRecorder.h"
#include "capture/SessionArchive.h"
#include "capture/CameraFormats.h"
#include "algorithms/RoiSignal.h"
#include "algorithms/PulseDisplay.h"
#include "algorithms/FaceSignalTracker.h"
#include <QTabWidget>
#include <QPainter>
#include <QtEndian>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace fv;
class TestDesktop : public QObject {
    Q_OBJECT
  private slots:
    void trackingContinuity() {
        QImage image(300,300,QImage::Format_RGB888); image.fill(QColor(120,100,80));
        FaceDetection face;face.box=QRect(30,20,240,260);face.unclippedBox=face.box;
        FaceSignalTracker tracker;tracker.detected({face},0);
        auto sample=tracker.sample(image,0);QVERIFY(sample.sample.valid);QCOMPARE(sample.regionMask,6);
        { QPainter p(&image);p.fillRect(QRect(30,20,240,90),Qt::black); }
        sample=tracker.sample(image,.1);QVERIFY(sample.sample.valid);QVERIFY(!sample.sample.breakContinuity);
        tracker.detected({},.2);sample=tracker.sample(image,.2);QVERIFY(!sample.sample.valid);QVERIFY(!sample.sample.breakContinuity);
        sample=tracker.sample(image,.3);QVERIFY(sample.sample.breakContinuity);
        tracker.detected({face},.4);QVERIFY(tracker.sample(image,.4).sample.valid);
        tracker.detected({face,face},.6);QVERIFY(tracker.sample(image,.6).sample.breakContinuity);
        tracker.reset();image.fill(QColor(120,100,80));
        face.box=QRect(30,100,240,200);face.unclippedBox=QRectF(30,100,240,260);face.visibleFraction=200./260.;
        tracker.detected({face},0);QVERIFY(tracker.sample(image,0).sample.valid);
        QVERIFY(emotionFaceCrop(image,face).isNull());
        face.unclippedBox=QRectF(30,190,240,260);face.box=QRect(30,190,240,110);face.visibleFraction=110./260.;
        tracker.detected({face},.2);QVERIFY(!tracker.sample(image,.2).sample.valid);
    }
    void roiAndRecentValue() {
        QImage frame(300,300,QImage::Format_RGB888); frame.fill(QColor(120,100,80));
        const QRect face(30,20,240,260);
        auto base=extractRoiSignal(frame,face,0); QVERIFY(base.sample.valid); QCOMPARE(base.regionMask,7);
        QPainter painter(&frame); painter.fillRect(QRect(30,20,240,90),Qt::black); painter.end();
        auto obscured=extractRoiSignal(frame,face,1); QVERIFY(obscured.sample.valid); QCOMPARE(obscured.regionMask,6);
        QCOMPARE(obscured.sample.g,100.); frame.fill(Qt::white);
        QVERIFY(!extractRoiSignal(frame,face,2).sample.valid);
        PulseResult good; good.heartRate.status="valid";good.heartRate.value=72;good.measuredAt=10;
        PulseResult bad;bad.heartRate.status="low_quality";
        auto recent=pulseForDisplay(bad,good,12);QCOMPARE(recent.heartRate.status,QString("recent"));QCOMPARE(recent.measuredAt,10.);
        QVERIFY(pulseForDisplay(bad,good,13.1).heartRate.status!="recent");
        QVERIFY(pulseForDisplay(bad,{},12).heartRate.status!="recent");
    }
    void cameraFormats() {
        const uchar bgr[] = {255,0,0,0, 0,0,255,0};
        auto image = decodeCameraFrame(bgr,8,1,2,-4,PixelFormat::Bgr24);
        QCOMPARE(image.pixelColor(0,0), QColor(Qt::red)); QCOMPARE(image.pixelColor(0,1),QColor(Qt::blue));
        const uchar yuyv[] = {16,128,235,128};
        image=decodeCameraFrame(yuyv,4,2,1,4,PixelFormat::Yuyv);
        QCOMPARE(image.pixelColor(0,0),QColor(Qt::black)); QCOMPARE(image.pixelColor(1,0),QColor(Qt::white));
        const uchar nv12[] = {16,235,235,16,128,128};
        for (auto fmt : {PixelFormat::Nv12,PixelFormat::Yuv420}) {
            image=decodeCameraFrame(nv12,6,2,2,2,fmt);
            QCOMPARE(image.pixelColor(0,0),QColor(Qt::black)); QCOMPARE(image.pixelColor(1,0),QColor(Qt::white));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,decodeCameraFrame(nv12,5,2,2,2,fmt));
        }
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,decodeCameraFrame(bgr,7,1,2,-4,PixelFormat::Bgr24));
    }
    void archiveBoundedAndReport() {
        QTemporaryDir dir; Measurement m; m.id=newId(); m.synthetic=true; m.status="partial";
        SessionArchive archive; archive.open(dir.path()+"/signals/"+m.id);
        for(int i=0;i<18000;++i) {
            Sample s{i/30.,120,100,80,i%7!=0}; archive.sample(m,s);
            if(i%6==0) { EmotionResult e; e.t=s.t; archive.emotion(m,e); }
        }
        archive.close(); QCOMPARE(m.totalSamples,18000); QCOMPARE(m.totalEmotions,3000);
        QVERIFY(m.samples.size()<3700); QVERIFY(m.emotions.size()<610);
        QCOMPARE(readFile(dir.path()+"/signals/"+m.id+"/signals.csv").count('\n'),18001);
        auto pdf=ReportWriter(dir.path()).generate(m); QVERIFY(ReportWriter::verify(QFileInfo(pdf).absolutePath()));
        QCOMPARE(readFile(dir.path()+"/reports/"+m.id+"/signals.csv").count('\n'),18001);
        QCOMPARE(readFile(dir.path()+"/reports/"+m.id+"/emotions.jsonl").count('\n'),3000);
        writeFile(dir.path()+"/reports/"+m.id+"/emotions.jsonl","changed");
        QVERIFY(!ReportWriter::verify(QFileInfo(pdf).absolutePath()));
    }
    void continuousAndRollover() {
        QTemporaryDir dir; Measurement m; m.synthetic=true;
        CaptureWorker worker(m,{},{},{},0); worker.configureArchive(dir.path()+"/signals");
        worker.configureRecording(dir.path()+"/video",true,2);
        QSignalSpy spy(&worker,&CaptureWorker::completed); worker.start();
        QTest::qWait(qEnvironmentVariableIsSet("FV_LONG_TEST") ? 65000 : 6400); QVERIFY(worker.isRunning()); QCOMPARE(spy.size(),0);
        worker.cancel(); QVERIFY(worker.wait(5000)); QCoreApplication::processEvents();
        QCOMPARE(spy.size(),1); auto result=qvariant_cast<Measurement>(spy.first().first());
        QVERIFY(result.recordings.size()>=3); QVERIFY(result.totalSamples>100);
        QVERIFY(result.recordingError.isEmpty()); QVERIFY(result.reason.contains("用户停止"));
        const auto output=qEnvironmentVariable("FV_ROLLOVER_ARTIFACTS");
        if(!output.isEmpty()) {
            QDir().mkpath(output);
            for(auto value:result.recordings) {
                const auto file=value.toObject()["file"].toString();
                QVERIFY(QFile::copy(dir.path()+"/video/"+file,output+"/"+file));
            }
            writeFile(output+"/result.json",QJsonDocument(result.json()).toJson());
        }
    }
    void cameraReconnect() {
        struct Fake final:ICameraSource {
            bool broken; int frames=0; explicit Fake(bool b):broken(b){}
            void open(const QString &) override {}
            QImage read() override { QThread::msleep(33); if(broken && ++frames>5) fail("模拟设备断开"); QImage f(160,120,QImage::Format_RGB888); f.fill(Qt::gray); return f; }
        };
        Measurement m; CaptureWorker worker(m,{},{},{},0); int opens=0;
        worker.setCameraFactory([&]{return std::make_unique<Fake>(++opens==1);});
        QSignalSpy spy(&worker,&CaptureWorker::completed); worker.start(); QTest::qWait(3500);
        const bool running=worker.isRunning(); worker.cancel(); QVERIFY(worker.wait(5000)); QCoreApplication::processEvents();
        QVERIFY(running); QVERIFY(opens>=2); QCOMPARE(spy.size(),1);
        auto result=qvariant_cast<Measurement>(spy.first().first()); QVERIFY(result.samples.size()>20); QVERIFY(result.status!="failed");
    }
    void settingsPermissions() {
        QTemporaryDir dir;
        Database db(dir.path()); AuthService auth(db);
        auth.bootstrap("admin", "Test-password-123");
        auto admin = auth.login("admin", "Test-password-123");
        for (const QString role : {"admin", "researcher", "operator", "user"}) {
            if (role != "admin") auth.createUser(admin, role, "Test-password-123", role);
            auto token = role == "admin" ? admin : auth.login(role, "Test-password-123");
            MainWindow window(db, auth, token, {});
            window.show();
            auto *settings = window.findChild<QDialog *>("settingsDialog");
            auto *tabs = window.findChild<QTabWidget *>("settingsTabs");
            QVERIFY(settings && tabs);
            QVERIFY(!settings->isVisible());
            QStringList names;
            for (int i = 0; i < tabs->count(); ++i) names.append(tabs->tabText(i));
            QCOMPARE(names.contains("模型中心"), role == "admin" || role == "researcher");
            QCOMPARE(names.contains("审计日志"), role == "admin");
            QCOMPARE(names.contains("账号管理"), role == "admin");
            QVERIFY(!names.contains("本机训练"));
            for (auto *box : window.findChildren<QCheckBox *>()) QVERIFY(!box->text().contains("知情"));
            QVERIFY(window.findChild<QCheckBox *>("recordingToggle")->isVisible());
            const auto screenshot = qEnvironmentVariable("FV_TEST_SCREENSHOT");
            if (!screenshot.isEmpty() && (role == "admin" || role == "user")) {
                settings->show(); QTest::qWait(50);
                QVERIFY(settings->grab().save(screenshot + "." + role + ".png"));
            }
        }
    }
    void recordingClock() {
        QTemporaryDir dir;
        VideoRecorder writer;
        QImage frame(160, 120, QImage::Format_RGB888); frame.fill(Qt::red);
        auto path = dir.path() + "/timing.avi";
        writer.open(path, frame, 2.0);
        frame.fill(Qt::blue); writer.append(frame, 4.0);
        writer.close();
        QCOMPARE(writer.frames(), 21);
        auto data = readFile(path);
        QCOMPARE(data.mid(0, 4), QByteArray("RIFF"));
        QCOMPARE(qFromLittleEndian<quint32>(data.constData() + 4), quint32(data.size() - 8));
        QCOMPARE(data.mid(8, 4), QByteArray("AVI "));
        QVERIFY(data.contains("idx1"));
        QVERIFY(!QFileInfo::exists(path + ".partial"));
        QVERIFY_EXCEPTION_THROWN(writer.open(path, frame, 0), std::runtime_error);
    }
    void lifecycle() {
        qRegisterMetaType<Measurement>();
        QTemporaryDir dir;
        Database db(dir.path());
        AuthService auth(db);
        auth.bootstrap("admin", "Test-password-123");
        auto token = auth.login("admin", "Test-password-123");
        MeasurementRepository repo(db, auth);
        MainWindow window(db, auth, token, {});
        window.show();
        auto *recording = window.findChild<QCheckBox *>("recordingToggle");
        QVERIFY(recording); recording->setChecked(true);
        window.startSyntheticForTest(22);
        QTimer::singleShot(3000, recording, [recording] { recording->setChecked(false); });
        QTimer::singleShot(5000, recording, [recording] { recording->setChecked(true); });
        QTRY_VERIFY_WITH_TIMEOUT(!repo.history(token).isEmpty(), 3000);
        const auto id = repo.history(token).first()["id"].toString();
        QTRY_VERIFY_WITH_TIMEOUT(repo.get(token, id).status != "measuring", 35000);
        auto result = repo.get(token, id);
        QCOMPARE(result.status, QString("completed"));
        QVERIFY(std::abs(result.pulse.heartRate.value - 72) < 1);
        QVERIFY(result.synthetic);
        QVERIFY2(result.recordingError.isEmpty(), qPrintable(result.recordingError));
        QCOMPARE(result.recordings.size(), 2);
        int clipNumber = 0;
        for (auto value : result.recordings) {
            auto clip = value.toObject();
            auto path = dir.path() + "/recordings/" + id + "/" + clip["file"].toString();
            QCOMPARE(fileHash(path), clip["sha256"].toString());
            QVERIFY(clip["frames"].toInt() > 10);
            const auto output = qEnvironmentVariable("FV_TEST_VIDEO_DIR");
            if (!output.isEmpty()) { QDir().mkpath(output); QVERIFY(QFile::copy(path, output + QString("/clip-%1.avi").arg(++clipNumber))); }
        }
        QVERIFY(ReportWriter::verify(dir.path() + "/reports/" + id));
        const auto screenshot = qEnvironmentVariable("FV_TEST_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            window.resize(1360, 1020);
            QTest::qWait(100);
            QVERIFY(window.grab().save(screenshot));
        }
        for (bool simulation : {true, false}) {
            auto m = repo.begin(token, "TEST", "test", {}, simulation);
            CaptureWorker worker(m, "invalid-camera-for-test", {}, {}, 30);
            worker.configureRecording(dir.path() + "/recordings/" + m.id, true);
            QSignalSpy spy(&worker, &CaptureWorker::completed);
            worker.start();
            if (simulation) {
                QTest::qWait(150);
                worker.cancel();
            }
            if (spy.isEmpty())
                QVERIFY(spy.wait(10000));
            QVERIFY(worker.wait(10000));
            auto ended = qvariant_cast<Measurement>(spy.first().first());
            QCOMPARE(ended.status, simulation ? QString("cancelled") : QString("failed"));
            QCOMPARE(ended.recordings.size(), simulation ? 1 : 0);
            QVERIFY(ended.recordingError.isEmpty());
            repo.finish(ended);
            auto pdf = ReportWriter(dir.path()).generate(ended);
            QVERIFY(ReportWriter::verify(QFileInfo(pdf).absolutePath()));
        }
    }
    void recordingFailureAndDefaultOff() {
        QTemporaryDir dir;
        auto occupied = dir.path() + "/occupied";
        writeFile(occupied, "not a directory");
        for (bool enabled : {false, true}) {
            Measurement m; m.synthetic = true;
            CaptureWorker worker(m, {}, {}, {}, 1);
            worker.configureRecording(occupied, enabled);
            QSignalSpy spy(&worker, &CaptureWorker::completed);
            worker.start();
            QVERIFY(spy.wait(5000)); QVERIFY(worker.wait(5000));
            auto result = qvariant_cast<Measurement>(spy.first().first());
            QVERIFY(!result.samples.isEmpty());
            QVERIFY(result.recordings.isEmpty());
            QCOMPARE(result.recordingError.isEmpty(), !enabled);
            QVERIFY(result.status != "failed");
        }
    }
};
QTEST_MAIN(TestDesktop)
#include "TestDesktop.moc"
