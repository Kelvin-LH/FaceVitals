#pragma once
#include <QDateTime>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QRect>
#include <QStringList>
#include <QVector>
#include <limits>
#include <stdexcept>

namespace fv {
inline void fail(const QString &message) {
    throw std::runtime_error(message.toUtf8().constData());
}
inline QString utcNow() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}
QString newId();
QByteArray readFile(const QString &path, qint64 limit = 128 * 1024 * 1024);
void writeFile(const QString &path, const QByteArray &data);
void copyFileChecked(const QString &source, const QString &destination);
QJsonObject readJson(const QString &path);
QString fileHash(const QString &path);
struct User {
    QString id, name, role;
    bool enabled = true;
};
struct Sample {
    double t = 0, r = 0, g = 0, b = 0;
    bool valid = false;
    bool breakContinuity = false;
    QString qualityReason;
};
struct Metric {
    double value = std::numeric_limits<double>::quiet_NaN();
    QString unit, status = "unsupported", reason;
    QJsonObject json() const;
};
struct PulseResult {
    Metric heartRate;
    double quality = 0;
    QVector<double> times, wave;
    double measuredAt = -1, windowSeconds = 0, coverage = 0, sampleRate = 0;
};
struct EmotionResult {
    QString label = "无法判断";
    QStringList labels;
    QVector<double> scores;
    QString status = "no_model";
    double t = 0;
    QJsonObject json() const;
};
struct Measurement {
    QString id, owner, subject, created, ended, source, status = "measuring", reason, modelId;
    bool synthetic = false;
    QJsonArray recordings;
    QString recordingError;
    bool streamed = false;
    qint64 totalSamples = 0, validSamples = 0, totalEmotions = 0;
    QJsonObject diagnostics;
    QString processing = "POS-1.4-bandpass / fixed-ROI-continuous / YuNet-letterbox / emotion-roll-EMA-0.45s";
    QVector<Sample> samples;
    QVector<EmotionResult> emotions;
    PulseResult pulse;
    QJsonObject json() const;
    static Measurement fromJson(const QJsonObject &o);
};
struct Preview {
    QImage image;
    QRect face;
    QString guidance;
    double elapsed = 0;
    EmotionResult emotion;
    PulseResult pulse;
    QVector<Sample> samples;
    QString recordingError;
};
} // namespace fv
Q_DECLARE_METATYPE(fv::Measurement)
