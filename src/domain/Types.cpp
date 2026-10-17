#include "Types.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>
#include <cmath>

namespace fv {
QString newId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
QByteArray readFile(const QString &path, qint64 limit) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > limit)
        fail("无法读取文件或文件过大：" + path);
    return f.readAll();
}
void writeFile(const QString &path, const QByteArray &data) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
        fail("文件写入失败：" + path);
}
void copyFileChecked(const QString &source, const QString &destination) {
    QFile input(source); QSaveFile output(destination);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) fail("无法复制信号文件。");
    char buffer[65536];
    for (;;) {
        const auto size = input.read(buffer, sizeof(buffer));
        if (size < 0) fail("读取信号文件失败。");
        if (size == 0) break;
        if (output.write(buffer, size) != size) fail("写入信号文件失败。");
    }
    if (!output.commit()) fail("保存信号文件失败。");
}
QJsonObject readJson(const QString &path) {
    QJsonParseError e;
    auto doc = QJsonDocument::fromJson(readFile(path, 16 * 1024 * 1024), &e);
    if (e.error != QJsonParseError::NoError || !doc.isObject())
        fail("JSON 格式无效：" + path);
    return doc.object();
}
QString fileHash(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        fail("无法计算文件哈希。");
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f))
        fail("读取哈希失败。");
    return QString::fromLatin1(h.result().toHex());
}
QJsonObject Metric::json() const {
    return {{"value", std::isfinite(value) ? QJsonValue(value) : QJsonValue(QJsonValue::Null)},
            {"unit", unit},
            {"status", status},
            {"reason", reason},
            {"source_type", status == "unsupported" ? "not_available" : "camera_estimate"}};
}
QJsonObject EmotionResult::json() const {
    QJsonArray a;
    for (double x : scores)
        a.append(x);
    return {{"t", t},
            {"label", label},
            {"labels", QJsonArray::fromStringList(labels)},
            {"scores", a},
            {"status", status}};
}
QJsonObject Measurement::json() const {
    QJsonArray rows, es, ts, ws;
    for (const auto &s : samples)
        rows.append(QJsonArray{s.t, s.r, s.g, s.b, s.valid, s.breakContinuity, s.qualityReason});
    for (const auto &e : emotions)
        es.append(e.json());
    for (double t : pulse.times)
        ts.append(t);
    for (double x : pulse.wave)
        ws.append(x);
    Metric unavailable;
    unavailable.reason = "当前普通 RGB 摄像头和模型未提供经过校准验证的此项测量";
    return {{"schema", 2},
            {"id", id},
            {"owner", owner},
            {"subject", subject},
            {"created", created},
            {"ended", ended},
            {"source", source},
            {"status", status},
            {"reason", reason},
            {"model_id", modelId},
            {"synthetic", synthetic},
            {"recordings", recordings},
            {"recording_error", recordingError},
            {"streamed", streamed},
            {"total_samples", totalSamples},
            {"valid_samples", validSamples},
            {"total_emotions", totalEmotions},
            {"diagnostics", diagnostics},
            {"pulse_window", QJsonObject{{"measured_at_seconds",pulse.measuredAt},{"seconds",pulse.windowSeconds},
                {"valid_coverage",pulse.coverage},{"sample_rate",pulse.sampleRate}}},
            {"preview_window_seconds", streamed ? QJsonValue(120) : QJsonValue(QJsonValue::Null)},
            {"samples", rows},
            {"emotions", es},
            {"wave_times", ts},
            {"wave", ws},
            {"sqi", pulse.quality},
            {"metrics", QJsonObject{{"heart_rate", pulse.heartRate.json()},
                                    {"blood_pressure", unavailable.json()},
                                    {"spo2", unavailable.json()},
                                    {"respiration", unavailable.json()}}},
            {"algorithm", processing}};
}
Measurement Measurement::fromJson(const QJsonObject &o) {
    Measurement m;
    m.processing = o["algorithm"].toString("POS-1.0 / adult research / 20s minimum");
    m.id = o["id"].toString();
    m.owner = o["owner"].toString();
    m.subject = o["subject"].toString();
    m.created = o["created"].toString();
    m.ended = o["ended"].toString();
    m.status = o["status"].toString();
    m.reason = o["reason"].toString();
    m.source = o["source"].toString();
    m.modelId = o["model_id"].toString();
    m.synthetic = o["synthetic"].toBool();
    m.recordings = o["recordings"].toArray();
    m.recordingError = o["recording_error"].toString();
    m.streamed = o["streamed"].toBool();
    m.totalSamples = o["total_samples"].toInteger();
    m.validSamples = o["valid_samples"].toInteger();
    m.totalEmotions = o["total_emotions"].toInteger();
    m.diagnostics = o["diagnostics"].toObject();
    const auto window = o["pulse_window"].toObject();
    m.pulse.measuredAt = window["measured_at_seconds"].toDouble(-1);
    m.pulse.windowSeconds = window["seconds"].toDouble();
    m.pulse.coverage = window["valid_coverage"].toDouble();
    m.pulse.sampleRate = window["sample_rate"].toDouble();
    for (auto v : o["samples"].toArray()) {
        auto a = v.toArray();
        if (a.size() >= 5)
            m.samples.append(
                {a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble(), a[4].toBool(),
                 a.size()>5 ? a[5].toBool() : !a[4].toBool(), a.size()>6 ? a[6].toString() : QString()});
    }
    for (auto v : o["emotions"].toArray()) {
        auto e = v.toObject();
        EmotionResult r;
        r.t = e["t"].toDouble();
        r.label = e["label"].toString();
        r.status = e["status"].toString();
        for (auto x : e["labels"].toArray())
            r.labels.append(x.toString());
        for (auto x : e["scores"].toArray())
            r.scores.append(x.toDouble());
        m.emotions.append(r);
    }
    auto h = o["metrics"].toObject()["heart_rate"].toObject();
    m.pulse.heartRate.value =
        h["value"].isDouble() ? h["value"].toDouble() : std::numeric_limits<double>::quiet_NaN();
    m.pulse.heartRate.status = h["status"].toString();
    m.pulse.heartRate.reason = h["reason"].toString();
    m.pulse.heartRate.unit = h["unit"].toString();
    m.pulse.quality = o["sqi"].toDouble();
    for (auto v : o["wave_times"].toArray())
        m.pulse.times.append(v.toDouble());
    for (auto v : o["wave"].toArray())
        m.pulse.wave.append(v.toDouble());
    return m;
}
} // namespace fv
