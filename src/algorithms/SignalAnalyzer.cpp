#include "SignalAnalyzer.h"
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
bool usable(const Sample &s) {
    return s.valid && std::isfinite(s.r) && std::isfinite(s.g) && std::isfinite(s.b) &&
           std::min({s.r, s.g, s.b}) >= 1 && std::max({s.r, s.g, s.b}) < 255;
}
struct Spectrum {
    double hz = 0, concentration = 0;
};
void bandpass(QVector<double> &signal) {
    // 二阶 Butterworth 正反向滤波；反射延拓降低端点瞬态，不引入相位延迟。
    const int pad=std::min(90,int(signal.size())-1);
    QVector<double> work; work.reserve(signal.size()+2*pad);
    for(int i=pad;i>0;--i) work.append(2*signal.first()-signal[i]);
    work+=signal;
    for(int i=1;i<=pad;++i) work.append(2*signal.last()-signal[signal.size()-1-i]);
    const auto filter=[&](double frequency,bool highpass) {
        const double k=std::tan(pi*frequency/30.), norm=1/(1+std::sqrt(2.)*k+k*k);
        const double b0=(highpass?1:k*k)*norm,b1=(highpass?-2:2)*b0,b2=b0;
        const double a1=2*(k*k-1)*norm,a2=(1-std::sqrt(2.)*k+k*k)*norm;
        double x1=work.first(),x2=x1,y1=highpass?0:x1,y2=y1;
        for(auto &x:work) { const double input=x; x=b0*input+b1*x1+b2*x2-a1*y1-a2*y2;
            x2=x1;x1=input;y2=y1;y1=x; }
    };
    filter(.7,true); filter(3.5,false); std::reverse(work.begin(),work.end());
    filter(.7,true); filter(3.5,false); std::reverse(work.begin(),work.end());
    signal=work.mid(pad,signal.size());
}
Spectrum spectrum(const QVector<double> &pulse, int begin, int end) {
    const int n = end - begin;
    QVector<double> windowed(n), powers;
    // 每个验证片段独立去趋势，避免均值和缓慢漂移支配频谱。
    double mean = 0, slopeNumerator = 0, slopeDenominator = 0;
    for (int i = 0; i < n; ++i) mean += pulse[begin + i];
    mean /= n;
    for (int i = 0; i < n; ++i) {
        const double x = i - (n - 1) / 2.;
        slopeNumerator += x * (pulse[begin + i] - mean);
        slopeDenominator += x * x;
    }
    const double slope = slopeDenominator > 0 ? slopeNumerator / slopeDenominator : 0;
    for (int i = 0; i < n; ++i)
        windowed[i] = (pulse[begin + i] - mean - slope * (i - (n - 1) / 2.)) *
                      (.5 - .5 * std::cos(2 * pi * i / (n - 1)));
    double peak = 0, total = 0;
    int pk = 0;
    for (int k = 0; k <= 280; ++k) {
        const double angle = 2 * pi * (.7 + .01 * k) / 30.;
        const double dc = std::cos(angle), ds = std::sin(angle);
        double c = 1, s = 0, re = 0, im = 0;
        for (double value : windowed) {
            re += value * c; im += value * s;
            const double nextC = c * dc - s * ds;
            s = s * dc + c * ds; c = nextC;
        }
        const double power = re * re + im * im;
        powers.append(power); total += power;
        if (power > peak) { peak = power; pk = k; }
    }
    Spectrum result;
    result.hz = .7 + .01 * pk;
    if (pk > 0 && pk + 1 < powers.size()) {
        const double denominator = powers[pk - 1] - 2 * powers[pk] + powers[pk + 1];
        if (denominator < -1e-12)
            result.hz += .01 * std::clamp(.5 * (powers[pk - 1] - powers[pk + 1]) / denominator, -.5, .5);
    }
    double band = 0;
    for (int k = std::max(0, pk - 10); k <= std::min(static_cast<int>(powers.size()) - 1, pk + 10); ++k)
        band += powers[k];
    result.concentration = total > 0 ? band / total : 0;
    return result;
}
} // namespace
PulseResult SignalAnalyzer::analyze(const QVector<Sample> &input) const {
    PulseResult out;
    out.heartRate.unit = "bpm";
    out.heartRate.status = "insufficient_duration";
    out.heartRate.reason = "需要至少 20 秒质量合格的信号。";
    if (input.size() < 2)
        return out;
    // 离脸、换人、断线由采集端明确标记边界。只桥接边界以内不超过 0.25 秒的质量缺口。
    const int last = static_cast<int>(input.size()) - 1;
    if (!std::isfinite(input.last().t) || !usable(input.last())) {
        out.heartRate.reason = "当前帧质量不足，等待有效画面恢复。";
        return out;
    }
    int start = last, nextValid = last;
    for (int i = last - 1; i >= 0; --i) {
        if (input[i + 1].breakContinuity || !std::isfinite(input[i].t) ||
            input[i + 1].t <= input[i].t || input[i + 1].t - input[i].t > .25 ||
            input.last().t - input[i].t > 30.) break;
        if (usable(input[i])) {
            if (input[nextValid].t - input[i].t > .25) break;
            start = nextValid = i;
        }
    }
    QVector<Sample> samples;
    QVector<double> steps;
    for (int i = start; i <= last; ++i) {
        if (usable(input[i])) samples.append(input[i]);
        if (i > start) steps.append(input[i].t - input[i - 1].t);
    }
    const double duration = input.last().t - input[start].t;
    out.windowSeconds = duration;
    if (duration <= 0 || steps.isEmpty()) return out;
    out.sampleRate = (samples.size() - 1) / duration;
    std::sort(steps.begin(), steps.end());
    const double nominalStep = steps[steps.size() / 2];
    double supported = 0;
    for (int i = 1; i < samples.size(); ++i)
        supported += std::min(samples[i].t - samples[i - 1].t, nominalStep * 1.5);
    out.coverage = std::min(supported / duration, double(samples.size()) / (last - start + 1));
    if (duration < 20.0) {
        out.heartRate.reason = QString("正在积累信号：%1 / 20 秒。").arg(duration, 0, 'f', 1);
        return out;
    }
    if (out.sampleRate < 15) {
        out.heartRate.reason = "有效采样率不足 15 FPS。";
        return out;
    }
    if (out.coverage < .90) {
        out.heartRate.status = "low_quality";
        out.heartRate.reason = "有效信号覆盖不足 90%，请保持稳定。";
        return out;
    }
    // 限制局部坏帧密度，防止用长段插值凑出看似完整的波形。
    for (double begin = input[start].t; begin < input.last().t - 1.; begin += 5.) {
        const double end = std::min(begin + 5., input.last().t);
        int count = 0, good = 0;
        for (int i = start; i <= last; ++i)
            if (input[i].t >= begin && input[i].t <= end) { ++count; if (usable(input[i])) ++good; }
        if (!count || double(good) / count < .85 || good / (end - begin) < 12.) {
            out.heartRate.status = "low_quality";
            out.heartRate.reason = "局部信号缺失过多，请保持稳定。";
            return out;
        }
    }
    // 按真实时间戳重采样，均匀网格仅用于算法，不声称增加硬件采样精度。
    constexpr double fs = 30;
    const int n = static_cast<int>(std::floor(duration * fs));
    QVector<double> r(n), g(n), b(n), pulse(n, 0), weight(n, 0);
    int j = 0;
    for (int i = 0; i < n; ++i) {
        double t = input[start].t + i / fs;
        while (j + 1 < samples.size() - 1 && samples[j + 1].t < t)
            ++j;
        double a = (t - samples[j].t) / (samples[j + 1].t - samples[j].t);
        r[i] = samples[j].r * (1 - a) + samples[j + 1].r * a;
        g[i] = samples[j].g * (1 - a) + samples[j + 1].g * a;
        b[i] = samples[j].b * (1 - a) + samples[j + 1].b * a;
        if (!std::isfinite(r[i] + g[i] + b[i]) || std::min({r[i], g[i], b[i]}) < 1) {
            out.heartRate.reason = "颜色信号无效。";
            return out;
        }
    }
    // 逐帧去除共同乘性亮度，再做 POS；上述插值仅覆盖已限定的小缺口。
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
    bandpass(pulse);
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
    const auto whole = spectrum(pulse, 0, n);
    out.quality = whole.concentration;
    if (out.quality < .45 || whole.hz < .73 || whole.hz > 3.47) {
        out.heartRate.status = "low_quality";
        out.heartRate.reason = "频谱不够集中或峰值处于分析边界。";
        return out;
    }
    // 前后半窗必须分别有集中频峰且相互一致，拒绝短暂伪峰或窗口内突变。
    const auto first = spectrum(pulse, 0, n / 2), second = spectrum(pulse, n / 2, n);
    if (first.concentration < .45 || second.concentration < .45 ||
        std::abs(first.hz - second.hz) > .15 || std::abs(first.hz - whole.hz) > .15 ||
        std::abs(second.hz - whole.hz) > .15) {
        out.heartRate.status = "low_quality";
        out.heartRate.reason = "前后时间段的脉搏频率不一致，请减少运动并等待稳定。";
        return out;
    }
    out.heartRate.value = std::round(whole.hz * 600) / 10;
    out.heartRate.status = "valid";
    out.heartRate.reason = "POS 研究估计；通过覆盖率与时间一致性检查，质量分数不是准确率。";
    out.measuredAt = input.last().t;
    return out;
}
} // namespace fv
