#include "EmotionRadar.h"
#include <QPainter>
#include <QPolygonF>
#include <cmath>
#include <algorithm>

namespace fv {
void EmotionRadar::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor("#0a1b3e"));
    const QStringList labels = result_.labels.isEmpty() ? QStringList{"愤怒", "轻蔑", "厌恶", "恐惧", "高兴", "中性", "悲伤", "惊讶"} : result_.labels;
    const int n = labels.size();
    if (n < 2) return;
    const bool available = result_.scores.size() == n && (result_.status=="valid" || result_.status=="uncertain");
    const QPointF center(width() / 2., height() / 2. + 2);
    const double radius = std::max(20.0, std::min(width(), height()) * .5 - 74);
    auto vertex = [&](int k, double scale) {
        const double a = -1.57079632679 + k * 6.28318530718 / n;
        return center + QPointF(std::cos(a), std::sin(a)) * radius * scale;
    };
    p.setPen(QColor("#93caee"));
    p.drawText(QRectF(0, 3, width(), 22), Qt::AlignCenter, "表情分布 / 模型分数");
    for (int ring = 1; ring <= 4; ++ring) {
        QPolygonF polygon;
        for (int k = 0; k < n; ++k) polygon << vertex(k, ring / 4.);
        p.setPen(QPen(QColor(ring == 4 ? "#377caf" : "#214768"), 1));
        p.setBrush(Qt::NoBrush); p.drawPolygon(polygon);
    }
    for (int k = 0; k < n; ++k) {
        p.setPen(QColor("#214768")); p.drawLine(center, vertex(k, 1));
        const auto pos = vertex(k, (radius + 27) / radius);
        const QString score = available ? QString::number(result_.scores[k] * 100, 'f', 0) + "%" : "—";
        p.setPen(QColor("#abd7f4"));
        p.drawText(QRectF(pos.x() - 35, pos.y() - 20, 70, 42), Qt::AlignCenter, labels[k] + "\n" + score);
    }
    if (available) {
        QPolygonF shape;
        for (int k = 0; k < n; ++k)
            shape << vertex(k, std::isfinite(result_.scores[k]) ? std::clamp(result_.scores[k], 0.0, 1.0) : 0);
        p.setPen(QPen(QColor("#55e5ff"), 2)); p.setBrush(QColor(47, 181, 255, 70)); p.drawPolygon(shape);
        p.setBrush(QColor("#91f3ff"));
        for (const auto &point : shape) p.drawEllipse(point, 2.5, 2.5);
    } else {
        p.setPen(QColor("#6fa3c6"));
        p.drawText(QRectF(0, center.y() - 12, width(), 24), Qt::AlignCenter,
                   result_.status == "synthetic" ? "模拟模式 · 无表情数据" : result_.status=="poor_image" ? "面部图像质量不足" : "等待人脸与模型输出");
    }
    p.setPen(QColor("#6c8fae"));
    p.drawText(QRectF(0, height() - 22, width(), 20), Qt::AlignCenter, "范围 0–100% · 未经概率校准");
}
}
