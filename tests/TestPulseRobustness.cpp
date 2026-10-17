// Frozen v0.5 analyzer; source SHA-256: 651CD808D3B8ECA67D9C244A3E0989D8C3B5387A6786C3A58E4117EDC74FBA17
// Keep unchanged: this is the comparison baseline, not the production implementation.
#include "algorithms/SignalAnalyzer.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace fv::frozenV05 {
namespace {
constexpr double pi = 3.14159265358979323846;
double deviation(const QVector<double> &v) {
    double mu = std::accumulate(v.begin(), v.end(), 0.0) / v.size(), s = 0;
    for (double x : v)
        s += (x - mu) * (x - mu);
    return std::sqrt(s / v.size());
}
} // namespace
PulseResult analyze(const QVector<Sample> &input) {
    PulseResult out;
    out.heartRate.unit = "bpm";
    out.heartRate.status = "insufficient_duration";
    out.heartRate.reason = "需要至少 20 秒连续、质量合格的信号。";
    if (input.size() < 2)
        return out;
    // 只用末尾连续有效段，不跨越离脸或长时间掉帧。
    int start = input.size() - 1;
    if (!input.last().valid)
        return out;
    while (start > 0 && input[start - 1].valid && input[start].t > input[start - 1].t &&
           input[start].t - input[start - 1].t <= 0.25 && input.last().t - input[start - 1].t <= 30.0)
        --start;
    const double duration = input.last().t - input[start].t;
    if (duration < 20.0)
        return out;
    const double fps = (input.size() - 1 - start) / duration;
    if (fps < 15) {
        out.heartRate.reason = "有效采样率不足 15 FPS。";
        return out;
    }
    // 按真实时间戳重采样，均匀网格仅用于算法，不声称增加硬件采样精度。
    constexpr double fs = 30;
    const int n = static_cast<int>(std::floor(duration * fs));
    QVector<double> r(n), g(n), b(n), pulse(n, 0), weight(n, 0);
    int j = start;
    for (int i = 0; i < n; ++i) {
        double t = input[start].t + i / fs;
        while (j + 1 < input.size() - 1 && input[j + 1].t < t)
            ++j;
        double a = (t - input[j].t) / (input[j + 1].t - input[j].t);
        r[i] = input[j].r * (1 - a) + input[j + 1].r * a;
        g[i] = input[j].g * (1 - a) + input[j + 1].g * a;
        b[i] = input[j].b * (1 - a) + input[j + 1].b * a;
        if (!std::isfinite(r[i] + g[i] + b[i]) || std::min({r[i], g[i], b[i]}) < 1) {
            out.heartRate.reason = "颜色信号无效。";
            return out;
        }
    }
    // 逐帧去除共同乘性亮度，再做 POS；不补造丢失帧或处理饱和像素。
    for (int i = 0; i < n; ++i) {
        const double intensity = (r[i] + g[i] + b[i]) / 3;
        r[i] /= intensity; g[i] /= intensity; b[i] /= intensity;
    }
    // POS：每个 1.6 秒窗口进行通道归一化与重叠相加。
    constexpr int length = 48;
    for (int end = length; end <= n; ++end) {
        double mr = 0, mg = 0, mb = 0;
        for (int i = end - length; i < end; ++i) {
            mr += r[i];
            mg += g[i];
            mb += b[i];
        }
        mr /= length;
        mg /= length;
        mb /= length;
        QVector<double> x(length), y(length), h(length);
        for (int k = 0; k < length; ++k) {
            int i = end - length + k;
            x[k] = g[i] / mg - b[i] / mb;
            y[k] = -2 * r[i] / mr + g[i] / mg + b[i] / mb;
        }
        double dy = deviation(y);
        if (dy < 1e-10)
            continue;
        double alpha = deviation(x) / dy, mu = 0;
        for (int k = 0; k < length; ++k) {
            h[k] = x[k] + alpha * y[k];
            mu += h[k];
        }
        mu /= length;
        for (int k = 0; k < length; ++k) {
            int i = end - length + k;
            pulse[i] += h[k] - mu;
            weight[i] += 1;
        }
    }
    for (int i = 0; i < n; ++i)
        if (weight[i] > 0)
            pulse[i] /= weight[i];
    double sd = deviation(pulse);
    if (sd < 1e-6) {
        out.heartRate.status = "low_quality";
        out.heartRate.reason = "未检测到可用脉搏变化。";
        return out;
    }
    double mu = std::accumulate(pulse.begin(), pulse.end(), 0.0) / n;
    for (int i = 0; i < n; ++i) {
        pulse[i] = (pulse[i] - mu) / sd;
        out.times.append(input[start].t + i / fs);
        out.wave.append(pulse[i]);
    }
    QVector<double> powers;
    double peak = 0, total = 0, best = 0;
    for (int k = 0; k <= 280; ++k) {
        double hz = 0.7 + 0.01 * k, re = 0, im = 0;
        for (int i = 0; i < n; ++i) {
            double w = 0.5 - 0.5 * std::cos(2 * pi * i / (n - 1)), a = 2 * pi * hz * i / fs;
            re += pulse[i] * w * std::cos(a);
            im += pulse[i] * w * std::sin(a);
        }
        double p = re * re + im * im;
        powers.append(p);
        total += p;
        if (p > peak) {
            peak = p;
            best = hz;
        }
    }
    int pk = static_cast<int>(std::round((best - .7) / .01));
    // 对频谱峰进行抛物线插值，减少 0.6 bpm 搜索步长导致的量化误差。
    if (pk > 0 && pk + 1 < powers.size()) {
        const double left = powers[pk - 1], center = powers[pk], right = powers[pk + 1];
        const double denominator = left - 2 * center + right;
        if (denominator < -1e-12)
            best += .01 * std::clamp(.5 * (left - right) / denominator, -.5, .5);
    }
    double band = 0;
    for (int k = std::max(0, pk - 10); k <= std::min(static_cast<int>(powers.size()) - 1, pk + 10); ++k)
        band += powers[k];
    out.quality = total > 0 ? band / total : 0;
    if (out.quality < .45 || best < .73 || best > 3.47) {
        out.heartRate.status = "low_quality";
        out.heartRate.reason = "频谱不够集中或峰值处于分析边界。";
        return out;
    }
    out.heartRate.value = std::round(best * 600) / 10;
    out.heartRate.status = "valid";
    out.heartRate.reason = "POS 研究估计；质量分数不是准确率。";
    return out;
}
} // namespace fv

#include <QJsonDocument>
#include <QFile>
#include <QtTest>
#include <random>

using namespace fv;
namespace {
constexpr double testPi = 3.14159265358979323846;
QVector<Sample> makePulse(double bpm, unsigned seed = 17, double fps = 30, double duration = 30) {
    std::mt19937 generator(seed);
    std::normal_distribution<double> noise(0, .045);
    QVector<Sample> samples;
    for (int i = 0; i <= int(fps * duration); ++i) {
        const double t = i / fps + .001 * std::sin(i * .7);
        const double pulse = std::sin(2 * testPi * bpm / 60 * t);
        const double light = 1 + .20 * std::sin(2 * testPi * 1.63 * t) + .003 * t;
        samples.append({t, light * (120 + .2 * pulse) + noise(generator),
                           light * (100 + .8 * pulse) + noise(generator),
                           light * (80 + .1 * pulse) + noise(generator), true});
    }
    return samples;
}
}
class TestPulseRobustness : public QObject {
    Q_OBJECT
  private slots:
    void frozenComparison();
    void realBoundariesAndCoverage();
    void rejectNoPulseAndNoise();
    void rejectChangingFrequency();
    void gradualFrequencyChange();
    void progressAndTimestampValidation();
};
void TestPulseRobustness::frozenComparison() {
    QJsonArray rows;
    int baselineValid = 0, currentValid = 0;
    double baselineError = 0, currentError = 0, maxError = 0;
    // 预先固定种子、频率和扰动：测试可用率仅针对数值仿真，不能代替真人准确率。
    for (unsigned seed : {17u, 42u, 93u, 121u, 181u, 207u, 253u, 317u, 401u, 509u}) {
        for (double bpm : {53.17, 71.83, 96.27, 137.13, 181.27}) {
            for (int mode = 0; mode < 4; ++mode) {
                auto samples = makePulse(bpm, seed);
                if (mode == 1 || mode == 2)
                    for (int i = 60; i < samples.size() - 10; i += 90)
                        for (int offset = 0; offset < (mode == 1 ? 1 : 3); ++offset) {
                            samples[i + offset].valid = false;
                            // 无效颜色不得参与插值，否则尖峰会污染整窗。
                            samples[i + offset].r = std::numeric_limits<double>::quiet_NaN();
                        }
                if (mode == 3)
                    for (int i = samples.size() - 30; i >= 60; i -= 90) samples.remove(i, 3);
                const auto old = frozenV05::analyze(samples), now = SignalAnalyzer().analyze(samples);
                if (old.heartRate.status == "valid") { ++baselineValid; baselineError += std::abs(old.heartRate.value - bpm); }
                if (now.heartRate.status == "valid") { ++currentValid; currentError += std::abs(now.heartRate.value - bpm); }
                maxError = std::max(maxError, std::abs(now.heartRate.value - bpm));
                rows.append(QJsonObject{{"seed", int(seed)}, {"bpm", bpm}, {"mode", mode},
                    {"baseline_status", old.heartRate.status}, {"baseline_bpm", std::isfinite(old.heartRate.value) ? QJsonValue(old.heartRate.value) : QJsonValue()},
                    {"current_status", now.heartRate.status}, {"current_bpm", std::isfinite(now.heartRate.value) ? QJsonValue(now.heartRate.value) : QJsonValue()},
                    {"window_seconds", now.windowSeconds}, {"coverage", now.coverage}, {"effective_fps", now.sampleRate}, {"quality", now.quality}});
                QCOMPARE(now.heartRate.status, QString("valid"));
                QVERIFY2(std::abs(now.heartRate.value - bpm) < .5,
                    qPrintable(QString("seed=%1 mode=%2 expected=%3 actual=%4").arg(seed).arg(mode).arg(bpm).arg(now.heartRate.value)));
                QVERIFY(now.coverage >= .90);
                QCOMPARE(now.measuredAt, samples.last().t);
                if (mode == 1 || mode == 2) QCOMPARE(old.heartRate.status, QString("insufficient_duration"));
            }
        }
    }
    const QJsonObject report{{"scope", "Deterministic numerical simulation only; no human accuracy claim."},
        {"baseline", "Frozen v0.5 SignalAnalyzer.cpp embedded unchanged apart from namespace and function name"},
        {"cases", rows.size()}, {"baseline_valid", baselineValid}, {"current_valid", currentValid},
        {"baseline_valid_mae_bpm", baselineError / baselineValid}, {"current_valid_mae_bpm", currentError / currentValid},
        {"current_max_error_bpm", maxError}, {"rows", rows}};
    qInfo().noquote() << QJsonDocument(QJsonObject{{"cases", rows.size()}, {"baseline_valid", baselineValid},
        {"current_valid", currentValid}, {"current_mae_bpm", currentError / currentValid}, {"current_max_error_bpm", maxError}}).toJson(QJsonDocument::Compact);
    const auto path = qEnvironmentVariable("FV_PULSE_REPORT");
    if (!path.isEmpty()) {
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        const auto bytes = QJsonDocument(report).toJson(); QCOMPARE(file.write(bytes), qint64(bytes.size()));
    }
}
void TestPulseRobustness::realBoundariesAndCoverage() {
    const auto clean = makePulse(72);
    auto samples = clean;
    samples[700].breakContinuity = true;
    QCOMPARE(SignalAnalyzer().analyze(samples).heartRate.status, QString("insufficient_duration"));
    samples[700].valid = false;
    QCOMPARE(SignalAnalyzer().analyze(samples).heartRate.status, QString("insufficient_duration"));
    samples = clean; samples[100].valid = false; samples[100].breakContinuity = true;
    QCOMPARE(SignalAnalyzer().analyze(samples).heartRate.status, QString("valid"));
    samples = clean;
    for (int i = 690; i < 710; ++i) samples[i].valid = false;
    QCOMPARE(SignalAnalyzer().analyze(samples).heartRate.status, QString("insufficient_duration"));
    samples = clean; samples.remove(690, 20);
    QCOMPARE(SignalAnalyzer().analyze(samples).heartRate.status, QString("insufficient_duration"));
    samples = clean;
    for (int i = 100; i < 880; i += 6) { samples[i].valid = false; samples[i + 1].valid = false; }
    QCOMPARE(SignalAnalyzer().analyze(samples).heartRate.status, QString("low_quality"));
    // 总覆盖仍超过 90%，但末段局部覆盖不足，不能放行。
    samples = clean;
    for (int i = 601; i < 720; i += 2) samples[i].valid = false;
    QCOMPARE(SignalAnalyzer().analyze(samples).heartRate.status, QString("low_quality"));
    samples = makePulse(72, 17, 10);
    QVERIFY(SignalAnalyzer().analyze(samples).heartRate.status != "valid");
    samples = clean; samples.last().valid = false;
    QVERIFY(SignalAnalyzer().analyze(samples).heartRate.status != "valid");
    samples = clean; samples.last().breakContinuity = true;
    QCOMPARE(SignalAnalyzer().analyze(samples).windowSeconds, 0.);
}
void TestPulseRobustness::rejectNoPulseAndNoise() {
    int baselineNoiseValid = 0, currentNoiseValid = 0;
    for (unsigned seed = 1; seed <= 100; ++seed) for (int mode = 0; mode < 4; ++mode) {
        std::mt19937 generator(seed); std::normal_distribution<double> noise(0, .12);
        QVector<Sample> samples;
        double nr = 0, ng = 0, nb = 0;
        for (int i = 0; i <= 900; ++i) {
            const double t = i / 30.;
            const double light = mode == 0 ? 1 : 1 + .30 * std::sin(2 * testPi * 1.3 * t);
            if (mode >= 2) {
                const double rho = mode == 3 ? .90 : 0;
                nr = rho * nr + noise(generator); ng = rho * ng + noise(generator); nb = rho * nb + noise(generator);
            }
            samples.append({t, 120 * light + nr, 100 * light + ng, 80 * light + nb, true});
        }
        const auto now = SignalAnalyzer().analyze(samples);
        if (now.heartRate.status == "valid") ++currentNoiseValid;
        if (frozenV05::analyze(samples).heartRate.status == "valid") ++baselineNoiseValid;
        QVERIFY2(now.heartRate.status != "valid", qPrintable(QString("no pulse seed=%1 mode=%2 bpm=%3").arg(seed).arg(mode).arg(now.heartRate.value)));
    }
    qInfo() << "No-pulse simulation 400 cases; baseline false outputs:" << baselineNoiseValid << "current:" << currentNoiseValid;
}
void TestPulseRobustness::rejectChangingFrequency() {
    for (unsigned seed : {17u, 42u, 93u}) {
        auto first = makePulse(72, seed), second = makePulse(126, seed + 1);
        for (int i = 450; i < first.size(); ++i) first[i] = second[i];
        const auto old = frozenV05::analyze(first), now = SignalAnalyzer().analyze(first);
        qInfo() << "Frequency-change case" << seed << "baseline" << old.heartRate.status << "current" << now.heartRate.status;
        QCOMPARE(now.heartRate.status, QString("low_quality"));
    }
}
void TestPulseRobustness::gradualFrequencyChange() {
    // 允许轻微连续变化，避免把所有正常心率波动都当作伪迹。
    for (double startBpm : {60., 90., 130.}) {
        auto samples = makePulse(startBpm);
        for (auto &sample : samples) {
            const double phase = 2 * testPi * (startBpm / 60. * sample.t + .5 * (8. / 30. / 60.) * sample.t * sample.t);
            const double pulse = std::sin(phase);
            sample.r = 120 + .2 * pulse; sample.g = 100 + .8 * pulse; sample.b = 80 + .1 * pulse;
        }
        const auto result = SignalAnalyzer().analyze(samples);
        QCOMPARE(result.heartRate.status, QString("valid"));
        QVERIFY(std::abs(result.heartRate.value - (startBpm + 4.)) < 1.);
    }
}
void TestPulseRobustness::progressAndTimestampValidation() {
    auto samples = makePulse(72, 17, 30, 10);
    auto result = SignalAnalyzer().analyze(samples);
    QCOMPARE(result.heartRate.status, QString("insufficient_duration"));
    QVERIFY(std::abs(result.windowSeconds - 10) < .01);
    QVERIFY(result.sampleRate > 29 && result.coverage > .99);
    samples = makePulse(72, 17, 30, 20.2);
    samples[300].valid = false;
    result = SignalAnalyzer().analyze(samples);
    QCOMPARE(result.heartRate.status, QString("valid"));
    QVERIFY(std::abs(result.heartRate.value - 72) < .5);
    samples = makePulse(72); samples[700].t = samples[699].t;
    QVERIFY(SignalAnalyzer().analyze(samples).heartRate.status != "valid");
    samples = makePulse(72); samples[700].t = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(SignalAnalyzer().analyze(samples).heartRate.status != "valid");
    samples = makePulse(72); samples.last().t = std::numeric_limits<double>::infinity();
    QVERIFY(SignalAnalyzer().analyze(samples).heartRate.status != "valid");
}
QTEST_GUILESS_MAIN(TestPulseRobustness)
#include "TestPulseRobustness.moc"
