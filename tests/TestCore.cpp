#include "algorithms/EmotionModel.h"
#include "algorithms/EmotionSmoother.h"
#include "algorithms/SignalAnalyzer.h"
#include "algorithms/IlluminationGate.h"
#include "reports/ReportWriter.h"
#include "services/MeasurementRepository.h"
#include "services/ModelRegistry.h"
#include "training/LocalTrainer.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <random>

namespace fv { PulseResult analyzeBaseline(const QVector<Sample> &); }

using namespace fv;
class TestCore : public QObject {
    Q_OBJECT
  private slots:
    void accountsAndIsolation();
    void rateLimitsAndDisable();
    void pulseAndQuality();
    void reportIntegrity();
    void trainingAndRegistry();
    void onnxSmoke();
    void bundledInitializationAndFace();
    void stabilityAndFrequency();
    void illuminationStress();
};
void TestCore::illuminationStress() {
    QJsonArray rows;
    for (int seed : {17, 42, 93}) for (double bpm : {54., 72., 117.}) for (int mode = 0; mode < 4; ++mode) {
        std::mt19937 rng(seed); std::normal_distribution<double> noise(0, .025);
        QVector<Sample> input;
        for (int i = 0; i < 900; ++i) {
            const double t = i / 30. + .001 * std::sin(i * .7);
            const double a = std::sin(2 * 3.141592653589793 * bpm / 60 * t);
            double light = mode == 0 ? 1 : mode == 1 ? .65 + .35 * t / 30 :
                mode == 2 ? 1 + .32 * std::sin(2 * 3.141592653589793 * 1.63 * t) : (t < 15 ? .7 : 1.15);
            input.append({t, light * (120 + .2*a) + noise(rng), light * (100 + .8*a) + noise(rng), light * (80 + .1*a) + noise(rng), true});
        }
        const auto old = analyzeBaseline(input), now = SignalAnalyzer().analyze(input);
        auto oldGated = input, newGated = input;
        IlluminationGate gate; double previous = -1;
        for (int i=0;i<input.size();++i) {
            const auto &s=input[i]; const double mean=(s.r+s.g+s.b)/3;
            oldGated[i].valid=previous<0 || std::abs(mean-previous)<=4; previous=mean;
            newGated[i].valid=gate.accept(s);
            newGated[i].breakContinuity=!newGated[i].valid;
        }
        const auto oldEnd=analyzeBaseline(oldGated), newEnd=SignalAnalyzer().analyze(newGated);
        rows.append(QJsonObject{{"seed",seed},{"bpm",bpm},{"mode",mode},{"baseline_status",old.heartRate.status},
            {"baseline_bpm",old.heartRate.value},{"new_status",now.heartRate.status},{"new_bpm",now.heartRate.value},
            {"baseline_with_gate",oldEnd.heartRate.status},{"new_with_gate",newEnd.heartRate.status}});
        if(mode==2) { QVERIFY(oldEnd.heartRate.status!="valid"); QCOMPARE(newEnd.heartRate.status,QString("valid")); }
        if(mode==3) QCOMPARE(newEnd.heartRate.status,QString("insufficient_duration"));
        QCOMPARE(now.heartRate.status, QString("valid"));
        QVERIFY2(std::abs(now.heartRate.value-bpm) < 1, qPrintable(QString("seed=%1 mode=%2 expected=%3 actual=%4").arg(seed).arg(mode).arg(bpm).arg(now.heartRate.value)));
    }
    const auto output = qEnvironmentVariable("FV_STRESS_REPORT");
    if (!output.isEmpty()) writeFile(output, QJsonDocument(rows).toJson());
    // 无脉搏只有亮度波动，不得生成心率；中断后必须重新积累。
    QVector<Sample> flat;
    for (int i=0; i<900; ++i) { double t=i/30., light=1+.3*std::sin(t*10); flat.append({t,120*light,100*light,80*light,true}); }
    QVERIFY(SignalAnalyzer().analyze(flat).heartRate.status != "valid");
    flat.last().valid=false;
    QCOMPARE(SignalAnalyzer().analyze(flat).heartRate.status, QString("insufficient_duration"));
    IlluminationGate gate; QVERIFY(gate.accept({0,120,100,80,true}));
    QVERIFY(!gate.accept({1,135,100,65,true})); // 总亮度相同、色温改变
}
void TestCore::stabilityAndFrequency() {
    // 非整数频率与不均匀时间戳，验证插值而非仅命中整数网格。
    for (double bpm : {53.17, 71.83, 96.27, 137.13}) {
        QVector<Sample> samples;
        for (int i = 0; i < 900; ++i) {
            double t = i / 30. + .002 * std::sin(i * .31);
            double a = std::sin(2 * 3.141592653589793 * bpm / 60 * t);
            samples.append({t, 120 + .2 * a, 100 + .8 * a, 80 + .1 * a, true});
        }
        auto r = SignalAnalyzer().analyze(samples);
        QCOMPARE(r.heartRate.status, QString("valid"));
        QVERIFY2(std::abs(r.heartRate.value - bpm) < .15, qPrintable(QString::number(r.heartRate.value)));
    }
    EmotionSmoother smoother;
    EmotionResult r;
    r.labels = {"a", "b"}; r.scores = {.9, .1}; r.t = 0;
    QCOMPARE(smoother.update(r).label, QString("a"));
    r.scores = {.1, .9}; r.t = .2;
    QVERIFY(smoother.update(r).label != "b");
    for (int i = 2; i < 8; ++i) { r.t = .2 * i; smoother.update(r); }
    r.t = 1.6;
    QCOMPARE(smoother.update(r).label, QString("b"));
    smoother.reset();
    r.scores = {.54, .46}; r.t = 2;
    QCOMPARE(smoother.update(r).status, QString("uncertain"));
}
void TestCore::accountsAndIsolation() {
    QTemporaryDir tmp;
    Database db(tmp.path());
    AuthService auth(db);
    QVERIFY(auth.needsSetup());
    auth.bootstrap("admin", "Long-admin-123");
    QVERIFY(!auth.needsSetup());
    QVERIFY_EXCEPTION_THROWN(auth.bootstrap("second", "Long-second-123"), std::runtime_error);
    auto admin = auth.login("admin", "Long-admin-123");
    auth.createUser(admin, "alice", "Long-alice-123", "user");
    auth.createUser(admin, "bob", "Long-bob-123", "user");
    auto alice = auth.login("alice", "Long-alice-123"), bob = auth.login("bob", "Long-bob-123");
    QVERIFY_EXCEPTION_THROWN(auth.createUser(alice, "rogue", "Long-rogue-123", "admin"), std::runtime_error);
    MeasurementRepository repo(db, auth);
    auto m = repo.begin(alice, "P001", "test", {}, false);
    m.status = "failed";
    m.reason = "no camera";
    m.ended = utcNow();
    repo.finish(m);
    QCOMPARE(repo.history(alice).size(), 1);
    QCOMPARE(repo.history(bob).size(), 0);
    QVERIFY_EXCEPTION_THROWN(repo.get(bob, m.id), std::runtime_error);
    QCOMPARE(repo.get(admin, m.id).reason, QString("no camera"));
    m.reason = "overwrite attempt";
    repo.finish(m);
    QCOMPARE(repo.get(alice, m.id).reason, QString("no camera"));
    auto interrupted = repo.begin(alice, "P002", "test", {}, false);
    auto recovered = repo.recoverInterrupted();
    QCOMPARE(recovered.size(), 1);
    QCOMPARE(repo.get(alice, interrupted.id).status, QString("failed"));
    auth.logout(alice);
    QVERIFY_EXCEPTION_THROWN(repo.history(alice), std::runtime_error);
}
void TestCore::rateLimitsAndDisable() {
    QTemporaryDir tmp;
    Database db(tmp.path());
    AuthService auth(db);
    auth.bootstrap("admin", "Long-admin-123");
    auto admin = auth.login("admin", "Long-admin-123");
    auth.createUser(admin, "a", "Long-user-123", "user");
    auto token = auth.login("a", "Long-user-123");
    auto uid = auth.require(token).id;
    auth.setEnabled(admin, uid, false);
    QVERIFY_EXCEPTION_THROWN(auth.require(token), std::runtime_error);
    auth.setEnabled(admin, uid, true);
    for (int i = 0; i < 5; ++i)
        QVERIFY_EXCEPTION_THROWN(auth.login("a", "wrong"), std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(auth.login("a", "Long-user-123"), std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(auth.setEnabled(admin, auth.require(admin).id, false), std::runtime_error);
    auth.changePassword(admin, "Long-admin-123", "New-password-123");
    QVERIFY_EXCEPTION_THROWN(auth.login("admin", "Long-admin-123"), std::runtime_error);
    QVERIFY(!auth.login("admin", "New-password-123").isEmpty());
}
void TestCore::pulseAndQuality() {
    QVector<Sample> signal;
    for (int i = 0; i < 900; ++i) {
        double t = i / 30., a = std::sin(2 * 3.141592653589793 * 1.2 * t);
        signal.append({t, 120 + .2 * a, 100 + .8 * a, 80 + .1 * a, true});
    }
    auto p = SignalAnalyzer().analyze(signal);
    QCOMPARE(p.heartRate.status, QString("valid"));
    QVERIFY(std::abs(p.heartRate.value - 72) < 1);
    QVERIFY(SignalAnalyzer().analyze(signal.mid(0, 200)).heartRate.status != "valid");
    auto gap = signal;
    gap[700].valid = false;
    gap[700].breakContinuity = true;
    QVERIFY(SignalAnalyzer().analyze(gap).heartRate.status != "valid");
    for (auto &s : signal) {
        s.r = 120;
        s.g = 100;
        s.b = 80;
    }
    QCOMPARE(SignalAnalyzer().analyze(signal).heartRate.status, QString("low_quality"));
    signal.last().valid = false;
    QVERIFY(SignalAnalyzer().analyze(signal).heartRate.status != "valid");
}
void TestCore::reportIntegrity() {
    QTemporaryDir tmp;
    Measurement m;
    m.id = newId();
    m.status = "failed";
    m.synthetic = true;
    m.subject = "测试受试者";
    m.reason = "测试失败也生成报告";
    m.created = utcNow();
    m.ended = m.created;
    ReportWriter writer(tmp.path());
    auto pdf = writer.generate(m);
    QVERIFY(readFile(pdf).startsWith("%PDF"));
    QVERIFY(ReportWriter::verify(QFileInfo(pdf).absolutePath()));
    auto hash = fileHash(pdf);
    QCOMPARE(writer.generate(m), pdf);
    QCOMPARE(fileHash(pdf), hash);
    QFile f(pdf);
    QVERIFY(f.open(QIODevice::Append));
    f.write("tampered");
    f.close();
    QVERIFY(!ReportWriter::verify(QFileInfo(pdf).absolutePath()));
    QVERIFY_EXCEPTION_THROWN(writer.generate(m), std::runtime_error);
    auto j = m.json();
    QCOMPARE(Measurement::fromJson(j).processing, m.processing);
    j["algorithm"] = "POS-1.0 / legacy";
    QCOMPARE(Measurement::fromJson(j).processing, QString("POS-1.0 / legacy"));
    QVERIFY(j["metrics"].toObject()["spo2"].toObject()["value"].isNull());
}
void TestCore::trainingAndRegistry() {
    QTemporaryDir tmp;
    QJsonArray rows;
    QJsonArray labels{"neutral", "happy"};
    for (int i = 0; i < 8; ++i) {
        QImage image(32, 32, QImage::Format_RGB888);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) {
                int v = (i % 2 ? x : y) * 6 + i;
                image.setPixelColor(x, y, QColor(v, v, v));
            }
        QString path = QString("image%1.png").arg(i);
        QVERIFY(image.save(tmp.path() + "/" + path));
        rows.append(QJsonObject{{"path", path},
                                {"subject", QString("s%1").arg(i)},
                                {"split", i < 4 ? "train" : "val"},
                                {"label", i % 2}});
    }
    auto manifest = QJsonObject{
        {"schema", 1}, {"labels", labels}, {"authorization", "Synthetic test only"}, {"rows", rows}};
    QString path = tmp.path() + "/dataset.json";
    writeFile(path, QJsonDocument(manifest).toJson());
    std::atomic_bool cancelled{false};
    auto dest = LocalTrainer::train(path, tmp.path() + "/runs", 8, cancelled, [](QString) {});
    QVERIFY(QFileInfo::exists(dest + "/training.json"));
    auto model = makeEmotionModel(dest);
    QCOMPARE(model->predict(QImage(tmp.path() + "/image0.png")).scores.size(), 2);
    Database db(tmp.path() + "/app");
    AuthService auth(db);
    auth.bootstrap("admin", "Long-admin-123");
    auto token = auth.login("admin", "Long-admin-123");
    ModelRegistry registry(db, auth);
    auto id = registry.importModel(token, dest);
    registry.activate(token, id);
    QCOMPARE(registry.activeId(), id);
    QVERIFY_EXCEPTION_THROWN(registry.importModel(token, dest), std::runtime_error);
    auto original = readJson(dest + "/manifest.json");
    original["id"] = "rollback-test-v2";
    writeFile(dest + "/manifest.json", QJsonDocument(original).toJson());
    auto second = registry.importModel(token, dest);
    registry.activate(token, second);
    QCOMPARE(registry.activeId(), second);
    registry.activate(token, id);
    QCOMPARE(registry.activeId(), id);
    cancelled = true;
    QVERIFY_EXCEPTION_THROWN(LocalTrainer::train(path, tmp.path() + "/runs", 1, cancelled, [](QString) {}),
                             std::runtime_error);
    cancelled = false;
    original["id"] = "bad";
    original["file"] = "../model.json";
    writeFile(dest + "/manifest.json", QJsonDocument(original).toJson());
    QVERIFY_EXCEPTION_THROWN(registry.importModel(token, dest), std::runtime_error);
    auto leaked = rows[4].toObject();
    leaked["subject"] = "s0";
    rows[4] = leaked;
    manifest["rows"] = rows;
    writeFile(path, QJsonDocument(manifest).toJson());
    QVERIFY_EXCEPTION_THROWN(LocalTrainer::train(path, tmp.path() + "/runs", 1, cancelled, [](QString) {}),
                             std::runtime_error);
}
void TestCore::onnxSmoke() {
#ifdef FV_HAS_ONNX
    const QString dir = QString::fromUtf8(FV_SOURCE_DIR) + "/models/bootstrap";
    auto emotion = makeEmotionModel(dir);
    QImage image(640, 480, QImage::Format_RGB888);
    image.fill(Qt::gray);
    auto result = emotion->predict(image);
    QCOMPARE(result.scores.size(), 8);
    double total = 0;
    for (double s : result.scores) {
        QVERIFY(std::isfinite(s));
        total += s;
    }
    QVERIFY(std::abs(total - 1) < 1e-5);
    auto face = makeFaceDetector(dir + "/face.onnx");
    QVERIFY(face->detect(image).isEmpty());
#else
    QSKIP("ONNX optional backend not built");
#endif
}
void TestCore::bundledInitializationAndFace() {
#ifdef FV_HAS_ONNX
    QTemporaryDir tmp;
    Database db(tmp.path() + "/data"); AuthService auth(db); ModelRegistry registry(db, auth);
    const auto assets = QString::fromUtf8(FV_SOURCE_DIR) + "/models/bootstrap";
    registry.ensureBundledModel(assets);
    QCOMPARE(registry.activeId(), QString("hsemotion-b0-afew-1"));
    const auto events = db.scalar("SELECT count(*) FROM audit").toInt();
    registry.ensureBundledModel(assets);
    QCOMPARE(db.scalar("SELECT count(*) FROM audit").toInt(), events);
    // 已导入但尚未发布的旧安装也自动完成初始化。
    db.execute("DELETE FROM settings WHERE key='active_emotion'");
    registry.ensureBundledModel(assets);
    QCOMPARE(registry.activeId(), QString("hsemotion-b0-afew-1"));
    QImage image(QString::fromUtf8(FV_SOURCE_DIR) + "/tests/fixtures/astronaut.png");
    QVERIFY(!image.isNull());
    image = image.scaled(640, 640, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    auto detector = makeFaceDetector(assets + "/face.onnx");
    auto faces = detector->detect(image);
    QCOMPARE(faces.size(), 1);
    QVERIFY(faces.first().width() >= 100);
    auto model = makeEmotionModel(registry.activePath());
    auto result = model->predict(image.copy(faces.first()));
    QCOMPARE(result.scores.size(), 8);
    QCOMPARE(result.status, QString("valid"));
    QCOMPARE(result.label, QString::fromUtf8("高兴"));
    EmotionSmoother smoother;
    for (int i = 0; i < 6; ++i) { result.t = .2 * i; result = smoother.update(result); }
    QCOMPARE(result.status, QString("valid"));
    qInfo().noquote() << QJsonDocument(result.json()).toJson(QJsonDocument::Compact);
    auth.bootstrap("admin", "Test-password-123");
    auto token = auth.login("admin", "Test-password-123");
    auth.createUser(token, "user", "Test-password-123", "user");
    auto user = auth.login("user", "Test-password-123");
    QCOMPARE(registry.list(user).size(), 1);
    QVERIFY_EXCEPTION_THROWN(registry.activate(user, registry.activeId()), std::runtime_error);
    const auto custom = tmp.path() + "/custom"; QDir().mkpath(custom);
    auto manifest = readJson(assets + "/manifest.json"); manifest["id"] = "custom-valid";
    writeFile(custom + "/manifest.json", QJsonDocument(manifest).toJson());
    QVERIFY(QFile::copy(assets + "/emotion.onnx", custom + "/emotion.onnx"));
    registry.importModel(token, custom); registry.activate(token, "custom-valid");
    registry.ensureBundledModel(assets);
    QCOMPARE(registry.activeId(), QString("custom-valid"));
    // 非固定包不能走自动安装通道；损坏的活动模型也不能静默降级。
    Database other(tmp.path() + "/other"); AuthService otherAuth(other); ModelRegistry otherRegistry(other, otherAuth);
    QVERIFY_EXCEPTION_THROWN(otherRegistry.ensureBundledModel(custom), std::runtime_error);
    writeFile(registry.activePath() + "/emotion.onnx", "broken");
    QVERIFY_EXCEPTION_THROWN(registry.ensureBundledModel(assets), std::runtime_error);
#else
    QSKIP("ONNX optional backend not built");
#endif
}
QTEST_MAIN(TestCore)
#include "TestCore.moc"
