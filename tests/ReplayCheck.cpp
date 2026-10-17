#include "algorithms/EmotionModel.h"
#include "algorithms/FaceSignalTracker.h"
#include "algorithms/RoiSignal.h"
#include "algorithms/SignalAnalyzer.h"
#include "domain/Types.h"
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMap>
#include <QProcess>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace fv { PulseResult analyzeBaselineV05(const QVector<Sample> &input); }
namespace {
QByteArray exact(QProcess &process, int count) {
    QByteArray result;
    QElapsedTimer timer; timer.start();
    while (result.size() < count) {
        result += process.read(count - result.size());
        if (result.size() == count) break;
        if (process.state() == QProcess::NotRunning)
            fv::fail("Decoder ended before its END1 packet: " + QString::fromUtf8(process.readAllStandardError()));
        if (timer.elapsed() > 60000) fv::fail("Decoder did not provide a complete frame within 60 seconds.");
        process.waitForReadyRead(1000);
    }
    return result;
}
quint32 u32(const QByteArray &data, int offset) {
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(data.constData() + offset));
}
quint64 u64(const QByteArray &data, int offset) {
    return qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(data.constData() + offset));
}
double f64(const QByteArray &data, int offset) {
    const auto bits = u64(data, offset); double result;
    static_assert(sizeof(result) == sizeof(bits)); std::memcpy(&result, &bits, sizeof(result));
    return result;
}
QJsonValue number(double value) {
    return std::isfinite(value) ? QJsonValue(value) : QJsonValue(QJsonValue::Null);
}
QString csvNumber(double value) {
    return std::isfinite(value) ? QString::number(value, 'f', 8) : QString();
}
QVector<double> loadReference(const QString &path) {
    const auto lines = QString::fromUtf8(fv::readFile(path, 16 * 1024 * 1024)).split('\n');
    if (lines.isEmpty() || lines.first().trimmed() != "Count,PPG,HR,SPO2")
        fv::fail("Expected the unmodified MPU Count,PPG,HR,SPO2 CSV.");
    QVector<double> values;
    for (int row = 1; row < lines.size(); ++row) {
        if (lines[row].trimmed().isEmpty()) continue;
        const auto fields = lines[row].trimmed().split(',');
        bool okCount = false, okHr = false;
        const qint64 count = fields.value(0).toLongLong(&okCount);
        const double hr = fields.value(2).toDouble(&okHr);
        if (fields.size() != 4 || !okCount || count != values.size() || !okHr || !std::isfinite(hr) || hr <= 0)
            fv::fail("Invalid or non-consecutive reference row " + QString::number(row));
        values.append(hr);
    }
    if (values.isEmpty()) fv::fail("Reference CSV has no rows.");
    return values;
}
struct ReferenceWindow { double mean = NAN; int count = 0; };
ReferenceWindow referenceMean(const QVector<double> &values, double fps, double start, double end) {
    ReferenceWindow out;
    if (!std::isfinite(start) || !std::isfinite(end) || end < start) return out;
    const qint64 first = qint64(std::ceil(start * fps - 1e-7));
    const qint64 last = qint64(std::floor(end * fps + 1e-7));
    if (first < 0 || last >= values.size() || last < first) return out;
    double total = 0;
    for (qint64 i = first; i <= last; ++i) { total += values[qsizetype(i)]; ++out.count; }
    out.mean = total / out.count;
    return out;
}
struct Statistics {
    int windows = 0, postWarmup = 0, valid = 0, validPostWarmup = 0, scored = 0, within5 = 0;
    double sumAbsolute = 0, sumSquared = 0, sumSigned = 0;
    QMap<QString, int> statusCounts;
    void add(const fv::PulseResult &pulse, double t, double reference) {
        ++windows; ++statusCounts[pulse.heartRate.status];
        if (t >= 20) ++postWarmup;
        if (pulse.heartRate.status != "valid" || !std::isfinite(pulse.heartRate.value)) return;
        ++valid; if (t >= 20) ++validPostWarmup;
        if (std::isfinite(reference)) {
            const double error = pulse.heartRate.value - reference;
            ++scored; sumAbsolute += std::abs(error); sumSquared += error * error; sumSigned += error;
            if (std::abs(error) <= 5) ++within5;
        }
    }
    QJsonObject json() const {
        QJsonObject statuses;
        for (auto i = statusCounts.cbegin(); i != statusCounts.cend(); ++i) statuses[i.key()] = i.value();
        return {{"windows", windows}, {"valid_windows", valid},
                {"valid_output_fraction", windows ? number(double(valid)/windows) : number(NAN)},
                {"post_20s_windows", postWarmup}, {"post_20s_valid_windows", validPostWarmup},
                {"post_20s_valid_output_fraction", postWarmup ? number(double(validPostWarmup)/postWarmup) : number(NAN)},
                {"scored_windows", scored}, {"exploratory_mae_bpm", scored ? number(sumAbsolute/scored) : number(NAN)},
                {"exploratory_rmse_bpm", scored ? number(std::sqrt(sumSquared/scored)) : number(NAN)},
                {"exploratory_bias_bpm", scored ? number(sumSigned/scored) : number(NAN)},
                {"exploratory_within_5bpm_fraction", scored ? number(double(within5)/scored) : number(NAN)},
                {"status_counts", statuses}};
    }
};
QJsonObject hashSources(const QString &root) {
    QJsonObject result;
    for (const QString &name : {"src/algorithms/SignalAnalyzer.cpp", "src/algorithms/RoiSignal.cpp",
         "src/algorithms/IlluminationGate.h", "src/algorithms/FaceSignalTracker.h", "src/algorithms/OnnxVision.cpp", "src/capture/CaptureWorker.cpp",
         "tests/PulseBaselineV05.cpp", "tests/ReplayCheck.cpp", "scripts/replay-video-frames.py"})
        result[name] = fv::fileHash(root + "/" + name);
    return result;
}
}

// 仅开发验证：Python 只解码；人脸、ROI 与心率分析均调用产品 C++ 实现。
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("Exploratory MPU replay; sensor alignment is unconfirmed.");
    parser.addHelpOption();
    parser.addOptions({QCommandLineOption("python", "Python executable with OpenCV.", "path", "python"),
        QCommandLineOption("decoder", "Path to scripts/replay-video-frames.py.", "path"),
        QCommandLineOption("video", "Unmodified public MP4.", "path"),
        QCommandLineOption("reference", "Unmodified MPU CSV.", "path"),
        QCommandLineOption("detector", "Face detector ONNX.", "path"),
        QCommandLineOption("output", "New or empty output directory.", "path"),
        QCommandLineOption("seconds", "Process source interval [0,seconds).", "seconds", "120")});
    parser.process(app);
    QProcess decoder;
    try {
        for (const auto *name : {"decoder", "video", "reference", "detector", "output"})
            if (!parser.isSet(name)) fv::fail(QString("Missing --") + name);
        bool validSeconds = false;
        const double seconds = parser.value("seconds").toDouble(&validSeconds);
        if (!validSeconds || seconds <= 0 || seconds > 3600) fv::fail("seconds must be in (0,3600].");
        const QString output = QDir(parser.value("output")).absolutePath();
        if (QDir(output).exists() && !QDir(output).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty())
            fv::fail("Output directory is not empty; refusing to overwrite an earlier run.");
        if (!QDir().mkpath(output)) fv::fail("Cannot create output directory.");
        QDir sourceRoot = QFileInfo(parser.value("decoder")).absoluteDir(); sourceRoot.cdUp();
        const auto sourceBefore = hashSources(sourceRoot.absolutePath());
        const auto reference = loadReference(parser.value("reference"));
        auto detector = fv::makeFaceDetector(parser.value("detector"));
        if (!detector) fv::fail("Face detector is unavailable in this build.");
        decoder.setProcessChannelMode(QProcess::SeparateChannels);
        decoder.start(parser.value("python"), {parser.value("decoder"), "--video", parser.value("video"),
                      "--seconds", QString::number(seconds, 'g', 12), "--fps", "30"});
        if (!decoder.waitForStarted(30000)) fv::fail("Cannot start the Python frame decoder: " + decoder.errorString());
        if (exact(decoder, 8) != "FVRP0001") fv::fail("Unknown replay stream protocol.");
        const quint32 metadataSize = u32(exact(decoder, 4), 0);
        if (metadataSize > 1024 * 1024) fv::fail("Replay metadata is too large.");
        QJsonParseError jsonError;
        const auto document = QJsonDocument::fromJson(exact(decoder, int(metadataSize)), &jsonError);
        if (jsonError.error != QJsonParseError::NoError || !document.isObject()) fv::fail("Invalid replay metadata.");
        const auto metadata = document.object();
        const double sourceFps = metadata["source_fps"].toDouble();
        if (sourceFps <= 0 || !std::isfinite(sourceFps)) fv::fail("Invalid source FPS.");
        if (metadata["source_frames"].toInteger() != reference.size())
            fv::fail("Frame count and reference Count length differ; cannot apply the stated index alignment assumption.");
        fv::SignalAnalyzer analyzer;
        fv::FaceSignalTracker tracker;
        QVector<fv::Sample> samples;
        QJsonArray windows;
        QByteArray sampleCsv = "source_frame,time_seconds,red,green,blue,valid,break_continuity,quality_reason,face_count,face_x,face_y,face_width,face_height,face_confidence,visible_fraction,roi_region_mask,roi_retained_fraction\n";
        QByteArray windowCsv = "time_seconds,algorithm,status,heart_rate_bpm,quality,window_start_seconds,window_end_seconds,reference_hr_mean_bpm,reference_rows,absolute_error_bpm,alignment_status\n";
        fv::FaceDetection detected;
        QRect face;
        int faceCount = 0, totalFrames = 0, validFrames = 0, hardBreaks = 0, detectorCalls = 0;
        double lastFace = -1, lastAnalysis = -1, lastTime = -1;
        quint64 lastSource = 0;
        QMap<QString,int> reasons;
        Statistics currentStats, baselineStats, sharedCurrent, sharedBaseline;
        QElapsedTimer elapsed; elapsed.start();
        for (;;) {
            const auto header = exact(decoder, 32);
            const auto tag = header.left(4);
            const quint64 sourceFrame = u64(header, 4);
            const double t = f64(header, 12);
            const quint32 width = u32(header, 20), height = u32(header, 24), bytes = u32(header, 28);
            if (tag == "END1") { if (bytes) fv::fail("END1 must have no payload."); break; }
            if (tag != "FRM1" || !width || !height || width > 8192 || height > 8192 ||
                quint64(bytes) != quint64(width) * height * 3 || bytes > 128 * 1024 * 1024)
                fv::fail("Malformed RGB frame packet.");
            if (!std::isfinite(t) || t < 0 || (totalFrames && (t <= lastTime || sourceFrame <= lastSource)))
                fv::fail("Replay frame timestamps/source indices must increase.");
            const auto rgb = exact(decoder, int(bytes));
            const QImage frame(reinterpret_cast<const uchar *>(rgb.constData()), int(width), int(height), int(width * 3), QImage::Format_RGB888);
            ++totalFrames; lastTime = t; lastSource = sourceFrame;
            // 复用当前 CaptureWorker 的检测周期、框平滑和运动判定。
            if (t - lastFace >= .2) {
                lastFace = t; ++detectorCalls;
                const auto faces = detector->detectDetailed(frame);
                tracker.detected(faces,t);
                faceCount=tracker.faceCount; detected=tracker.detection; face=detected.box;
            }
            const auto roi=tracker.sample(frame,t);
            const auto sample=roi.sample;
            const double retainedFraction=roi.retainedFraction;
            const int currentMask=roi.regionMask;
            samples.append(sample); validFrames += sample.valid; hardBreaks += sample.breakContinuity;
            ++reasons[sample.qualityReason];
            const QStringList values{QString::number(sourceFrame), csvNumber(t), csvNumber(sample.r), csvNumber(sample.g), csvNumber(sample.b),
                sample.valid ? "1" : "0", sample.breakContinuity ? "1" : "0", sample.qualityReason, QString::number(faceCount),
                QString::number(face.x()), QString::number(face.y()), QString::number(face.width()), QString::number(face.height()),
                csvNumber(detected.confidence), csvNumber(detected.visibleFraction), QString::number(currentMask), csvNumber(retainedFraction)};
            sampleCsv += values.join(',').toUtf8() + '\n';
            if (t - lastAnalysis >= 1) {
                lastAnalysis = t;
                const auto current = analyzer.analyze(samples);
                const auto baseline = fv::analyzeBaselineV05(samples);
                const auto appendWindow = [&](const QString &algorithm, const fv::PulseResult &pulse, Statistics &stats) {
                    const double start = pulse.times.isEmpty() ? NAN : pulse.times.first();
                    const auto ref = referenceMean(reference, sourceFps, start, t);
                    const bool isValid = pulse.heartRate.status == "valid" && std::isfinite(pulse.heartRate.value);
                    const double error = isValid && std::isfinite(ref.mean) ? std::abs(pulse.heartRate.value - ref.mean) : NAN;
                    stats.add(pulse, t, ref.mean);
                    const QJsonObject row{{"t", t}, {"algorithm", algorithm}, {"status", pulse.heartRate.status},
                        {"reason", pulse.heartRate.reason}, {"bpm", number(pulse.heartRate.value)}, {"quality", pulse.quality},
                        {"window_start", number(start)}, {"window_end", t}, {"window_seconds", number(std::isfinite(start) ? t - start : NAN)},
                        {"reference_hr_mean", number(ref.mean)}, {"reference_count", ref.count},
                        {"absolute_error_bpm", number(error)}, {"alignment_status", "unconfirmed"}};
                    windows.append(row);
                    windowCsv += QStringList{csvNumber(t), algorithm, pulse.heartRate.status, csvNumber(pulse.heartRate.value),
                        csvNumber(pulse.quality), csvNumber(start), csvNumber(t), csvNumber(ref.mean), QString::number(ref.count),
                        csvNumber(error), "unconfirmed"}.join(',').toUtf8() + '\n';
                    return ref.mean;
                };
                const double currentRef = appendWindow("current", current, currentStats);
                const double baselineRef = appendWindow("baseline_v05", baseline, baselineStats);
                if (current.heartRate.status == "valid" && baseline.heartRate.status == "valid") {
                    sharedCurrent.add(current, t, currentRef); sharedBaseline.add(baseline, t, baselineRef);
                }
            }
        }
        if (!decoder.waitForFinished(10000) || decoder.exitStatus() != QProcess::NormalExit || decoder.exitCode() != 0)
            fv::fail("Decoder failed: " + QString::fromUtf8(decoder.readAllStandardError()));
        if (!totalFrames) fv::fail("Replay produced no frames.");
        QJsonObject reasonJson;
        for (auto i = reasons.cbegin(); i != reasons.cend(); ++i) reasonJson[i.key()] = i.value();
        const auto sourceAfter = hashSources(sourceRoot.absolutePath());
        const QJsonObject summary{{"created_utc", fv::utcNow()}, {"alignment_status", "unconfirmed"},
            {"claim_scope", "exploratory replay only; not a validated physiological accuracy result"},
            {"reference_source", "MPU CSV HR column from contact pulse oximeter; no model-generated reference"},
            {"alignment_assumption", "CSV Count equals source video frame index; reference time = Count / source_fps; offset fixed at zero, not optimized"},
            {"decoder_metadata", metadata}, {"decoder_stderr", QString::fromUtf8(decoder.readAllStandardError())},
            {"requested_seconds", seconds}, {"processed_last_timestamp", lastTime}, {"source_last_frame", qint64(lastSource)},
            {"processed_frames", totalFrames}, {"valid_signal_frames", validFrames}, {"valid_signal_fraction", double(validFrames)/totalFrames},
            {"hard_continuity_break_frames", hardBreaks}, {"frame_reason_counts", reasonJson}, {"detector_calls", detectorCalls},
            {"elapsed_seconds", elapsed.elapsed()/1000.}, {"current", currentStats.json()}, {"baseline_v05", baselineStats.json()},
            {"both_valid_current", sharedCurrent.json()}, {"both_valid_baseline_v05", sharedBaseline.json()},
            {"video_sha256", fv::fileHash(parser.value("video"))}, {"reference_sha256", fv::fileHash(parser.value("reference"))},
            {"detector_sha256", fv::fileHash(parser.value("detector"))}, {"executable_sha256", fv::fileHash(app.applicationFilePath())},
            {"source_hashes_before", sourceBefore}, {"source_hashes_after", sourceAfter}, {"source_unchanged_during_run", sourceBefore == sourceAfter},
            {"pipeline_differences", QJsonArray{"Offline source timestamps and fixed 30 FPS subsampling replace live camera arrival timestamps.",
                 "No camera capture backend, reconnect, recorder, emotion inference, session persistence or held display values.",
                 "Face detection scheduling, box smoothing, movement thresholds, ROI extraction and illumination gate mirror CaptureWorker at the recorded source hash.",
                 "Both analyzers receive exactly the same extracted RGB samples and valid flags; this isolates algorithm changes, not the complete old v0.5 ROI/capture pipeline."}},
            {"windows", windows}};
        fv::writeFile(output + "/samples.csv", sampleCsv);
        fv::writeFile(output + "/windows.csv", windowCsv);
        fv::writeFile(output + "/summary.json", QJsonDocument(summary).toJson());
        const auto statLine = [](const QString &name, const Statistics &stats) {
            return QString("| %1 | %2 / %3 | %4 / %5 | %6 | %7 |\n").arg(name).arg(stats.valid).arg(stats.windows)
                .arg(stats.validPostWarmup).arg(stats.postWarmup).arg(stats.scored ? QString::number(stats.sumAbsolute/stats.scored,'f',2) : "无可评分窗口")
                .arg(stats.scored ? QString::number(std::sqrt(stats.sumSquared/stats.scored),'f',2) : "无可评分窗口");
        };
        QString report = "# MPU 公开视频回放探索\n\n**alignment_status=unconfirmed：本结果不能作为已验证的生理准确度。**\n\n";
        report += QString("处理视频前 %1 秒，获得 %2 帧颜色采样，其中 %3 帧有效（%4%）。参考为原始 CSV 的接触式血氧仪 HR 列；假设 Count 对应源视频帧序号并采用零偏移，未搜索或调整对齐。\n\n")
            .arg(seconds).arg(totalFrames).arg(validFrames).arg(100.*validFrames/totalFrames,0,'f',2);
        report += "| 算法 | 有效输出 / 全部窗口 | 20秒后有效 / 窗口 | 探索 MAE bpm | 探索 RMSE bpm |\n|---|---:|---:|---:|---:|\n";
        report += statLine("当前", currentStats) + statLine("冻结 v0.5 分析器", baselineStats);
        report += "\n各分析器只对自身有效窗口计算误差，覆盖范围可能不同；两者同时有效窗口结果另见 summary.json 的 both_valid 字段。\n\n";
        report += "## 流程范围\n\n采用当前 C++ 人脸检测、检测框平滑、运动门槛、多区域颜色提取和光照门槛，按源视频时间约 5 Hz 检测、30 Hz 采样、1 Hz 分析。两版分析器使用完全相同的 ROI 颜色序列。该对照只比较分析器，没有还原旧版采集/ROI流程。\n\n";
        report += "离线流程没有实时摄像头后端、到帧抖动、重连、录像、表情推理开销、数据库或界面暂留数值。统计的是本次分析实际得到的 valid 输出。\n\n";
        report += "逐帧有效原因见 samples.csv，逐窗口读数及参考均值见 windows.csv，来源哈希和完整统计见 summary.json。媒体配对/时间同步缺口见上一级 README_核查.md。本目录不进入软件分发包。\n";
        fv::writeFile(output + "/report.md", report.toUtf8());
        std::cout << QJsonDocument(QJsonObject{{"output", output}, {"alignment_status", "unconfirmed"},
            {"current", currentStats.json()}, {"baseline_v05", baselineStats.json()}, {"frame_reason_counts", reasonJson}}).toJson().constData();
        return sourceBefore == sourceAfter ? 0 : 3;
    } catch (const std::exception &error) {
        if (decoder.state() != QProcess::NotRunning) { decoder.kill(); decoder.waitForFinished(3000); }
        std::cerr << error.what() << '\n';
        return 1;
    }
}
