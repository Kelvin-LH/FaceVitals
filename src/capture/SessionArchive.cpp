#include "SessionArchive.h"
#include <QDir>
#include <QJsonDocument>
#include <algorithm>

namespace fv {
void SessionArchive::write(QFile &file, const QByteArray &data) {
    if (file.write(data) != data.size()) fail("信号保存失败，请检查磁盘空间。");
}
void SessionArchive::open(const QString &directory) {
    if (directory.isEmpty()) return;
    if (!QDir().mkpath(directory)) fail("无法创建信号目录。");
    signals_.setFileName(directory + "/signals.csv");
    emotions_.setFileName(directory + "/emotions.jsonl");
    if (!signals_.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        !emotions_.open(QIODevice::WriteOnly | QIODevice::NewOnly)) fail("无法创建信号文件。");
    write(signals_, "time_seconds,red,green,blue,valid,break_continuity,quality_reason\n");
}
void SessionArchive::sample(Measurement &m, const Sample &value) {
    if (signals_.isOpen()) {
        write(signals_, QByteArray::number(value.t, 'f', 6) + "," + QByteArray::number(value.r, 'f', 6) + "," +
            QByteArray::number(value.g, 'f', 6) + "," + QByteArray::number(value.b, 'f', 6) + "," + (value.valid ? "1," : "0,") +
            (value.breakContinuity ? "1," : "0,") + value.qualityReason.toLatin1() + "\n");
        m.streamed = true;
    }
    m.samples.append(value); ++m.totalSamples;
    m.validSamples += value.valid;
    const auto key = value.valid ? QString("valid") : value.qualityReason.isEmpty() ? QString("unspecified") : value.qualityReason;
    auto reasons = m.diagnostics["frame_quality_counts"].toObject();
    reasons[key] = reasons[key].toInteger() + 1; m.diagnostics["frame_quality_counts"] = reasons;
    if(value.breakContinuity) m.diagnostics["continuity_breaks"] = m.diagnostics["continuity_breaks"].toInteger()+1;
    if (m.streamed && value.t - lastFlush_ >= 1) {
        if (!signals_.flush() || !emotions_.flush()) fail("信号保存失败，请检查磁盘空间。");
        const double cutoff = value.t - 120;
        auto s = std::lower_bound(m.samples.cbegin(), m.samples.cend(), cutoff, [](const Sample &v, double t) { return v.t < t; });
        m.samples.remove(0, s - m.samples.cbegin());
        auto e = std::lower_bound(m.emotions.cbegin(), m.emotions.cend(), cutoff, [](const EmotionResult &v, double t) { return v.t < t; });
        m.emotions.remove(0, e - m.emotions.cbegin());
        lastFlush_ = value.t;
    }
}
void SessionArchive::emotion(Measurement &m, const EmotionResult &value) {
    if (emotions_.isOpen()) write(emotions_, QJsonDocument(value.json()).toJson(QJsonDocument::Compact) + '\n');
    m.emotions.append(value); ++m.totalEmotions;
}
void SessionArchive::close() {
    for (auto *file : {&signals_, &emotions_}) {
        if (file->isOpen()) {
            if (!file->flush()) fail("信号文件未能完整保存。");
            file->close();
        }
    }
}
}
