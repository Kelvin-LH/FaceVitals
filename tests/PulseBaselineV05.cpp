#include "algorithms/SignalAnalyzer.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace fv {
namespace {
constexpr double pi = 3.14159265358979323846;
double deviation(const QVector<double> &v) {
    double mu = std::accumulate(v.begin(), v.end(), 0.0) / v.size(), s = 0;
    for (double x : v)
        s += (x - mu) * (x - mu);
    return std::sqrt(s / v.size());
}
} // namespace
PulseResult analyzeBaselineV05(const QVector<Sample> &input) {
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
