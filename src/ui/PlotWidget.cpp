#include "PlotWidget.h"
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace fv {
void PlotWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor("#040d24"));
    const QStringList names{"rPPG 脉搏波", "R  红色通道", "G  绿色通道", "B  蓝色通道"};
    const QColor colors[]{QColor("#37e6e0"), QColor("#ff688e"), QColor("#78ec9c"), QColor("#729dff")};
    const double last = samples_.isEmpty() ? 0 : samples_.last().t;
    const double end = std::max(30.0, last), begin = end - 30;
    const double left = 148, right = width() - 72.0;
    const double row = (height() - 27.0) / 4;
    for (int c = 0; c < 4; ++c) {
        const QRectF box(left, c * row + 6, right - left, row - 12);
        p.fillRect(QRectF(0, c * row, width(), row), QColor(c % 2 ? "#08142d" : "#061028"));
        for (int i = 0; i <= 30; ++i) {
            p.setPen(QColor(i % 5 ? "#0f223e" : "#1e4263"));
            double x = box.left() + box.width() * i / 30.;
            p.drawLine(QPointF(x, box.top()), QPointF(x, box.bottom()));
        }
        p.setPen(QColor("#16314d"));
        for (int i = 0; i <= 4; ++i) {
            double y = box.top() + box.height() * i / 4.;
            p.drawLine(QPointF(box.left(), y), QPointF(box.right(), y));
        }
        p.setPen(colors[c]);
        p.drawText(QRectF(12, c * row + 8, 130, 22), names[c]);
        QVector<double> times, values;
        if (c == 0) { times = pulse_.times; values = pulse_.wave; }
        else for (const auto &s : samples_) {
            if (s.t < begin) continue;
            times.append(s.t);
            values.append(!s.valid ? std::numeric_limits<double>::quiet_NaN() : c == 1 ? s.r : c == 2 ? s.g : s.b);
        }
        double lo = 1e100, hi = -1e100;
        for (int i = 0; i < values.size(); ++i)
            if (times[i] >= begin && std::isfinite(values[i])) { lo = std::min(lo, values[i]); hi = std::max(hi, values[i]); }
        p.setPen(QColor("#6283a4"));
        p.drawText(QRectF(12, c * row + 31, 130, 22), c == 0 ? "归一化 / a.u." : "ROI 均值 / 0–255");
        if (lo > hi || values.size() < 2) {
            p.setPen(QColor("#577997"));
            p.drawText(box, Qt::AlignCenter, c == 0 ? "等待连续有效片段 ≥ 20 秒" : "等待采集数据");
            continue;
        }
        { const double pad = std::max(.05, (hi - lo) * .12); lo -= pad; hi += pad; }
        p.drawText(QRectF(right + 6, box.top(), 62, 19), QString::number(hi, 'f', 2));
        p.drawText(QRectF(right + 6, box.bottom() - 18, 62, 19), QString::number(lo, 'f', 2));
        QPainterPath path;
        bool connected = false;
        for (int i = 0; i < values.size(); ++i) {
            if (times[i] < begin || !std::isfinite(values[i])) { connected = false; continue; }
            const QPointF pt(box.left() + (times[i] - begin) / 30. * box.width(), box.bottom() - (values[i] - lo) / (hi - lo) * box.height());
            if (connected && i && times[i] - times[i - 1] < .25) path.lineTo(pt); else path.moveTo(pt);
            connected = true;
        }
        p.save(); p.setClipRect(box);
        auto glow = colors[c]; glow.setAlpha(35);
        p.setPen(QPen(glow, 5)); p.drawPath(path);
        p.setPen(QPen(colors[c], 1.25)); p.drawPath(path);
        p.restore();
    }
    p.setPen(QColor("#7299bd"));
    for (int i = 0; i <= 6; ++i) {
        const double x = left + (right - left) * i / 6.;
        p.drawText(QRectF(x - 30, height() - 23, 60, 20), Qt::AlignCenter, QString::number(begin + i * 5, 'f', 0) + " s");
    }
}
} // namespace fv
