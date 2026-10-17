#include "ReportWriter.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace fv {
namespace {
QString textMetric(const Metric &m) {
    return m.status == "valid" && std::isfinite(m.value) ? QString::number(m.value, 'f', 1) + " " + m.unit
                                                         : "未输出";
}
void page(QPainter &p, const Measurement &m, int index) {
    p.fillRect(QRectF(0, 0, 800, 1132), Qt::white);
    p.save();
    p.setPen(QColor(229, 234, 240));
    p.setFont(QFont("Microsoft YaHei", 26));
    p.translate(150, 700);
    p.rotate(-35);
    p.drawText(QRectF(0, -250, 850, 380), Qt::AlignCenter | Qt::TextWordWrap,
               (m.synthetic ? "模拟数据 · 非人体测量" : "研究用途 · 非诊断报告") +
                   QString("\n%1").arg(m.id.left(18)));
    p.restore();
    p.setPen(QColor("#163044"));
    p.setFont(QFont("Microsoft YaHei", 22, QFont::Bold));
    p.drawText(QRectF(40, 30, 720, 50), "FaceVitals 测量报告");
    p.setFont(QFont("Microsoft YaHei", 10));
    p.setPen(QColor("#526375"));
    p.drawText(QRectF(40, 86, 720, 40), Qt::TextWordWrap,
               "报告编号：" + m.id + "    页码：" + QString::number(index) + " / 3");
    p.drawLine(40, 1060, 760, 1060);
    p.drawText(QRectF(40, 1072, 720, 38), Qt::TextWordWrap,
               "研究原型 / 模板 1.0 / " + m.ended +
                   "\n低质量或未经验证的指标不输出数值；水印不等同数字签名。");
}
void paragraph(QPainter &p, int y, const QString &s, int height = 60) {
    p.setPen(QColor("#24384a"));
    p.setFont(QFont("Microsoft YaHei", 12));
    p.drawText(QRectF(45, y, 710, height), Qt::TextWordWrap, s);
}
void plot(QPainter &p, const QRectF &box, const QVector<double> &times, const QVector<double> &values,
          const QString &title, const QColor &color) {
    p.setPen(QColor("#30465a"));
    p.setFont(QFont("Microsoft YaHei", 12, QFont::Bold));
    p.drawText(QRectF(box.x(), box.y() - 30, box.width(), 25), title);
    p.fillRect(box, QColor(246, 248, 251));
    p.setPen(QColor(217, 224, 230));
    p.drawRect(box);
    QVector<double> finite;
    for (double v : values)
        if (std::isfinite(v))
            finite.append(v);
    if (finite.size() < 2 || times.size() != values.size()) {
        p.drawText(box, Qt::AlignCenter, "无可用曲线");
        return;
    }
    double lo = *std::min_element(finite.begin(), finite.end()),
           hi = *std::max_element(finite.begin(), finite.end());
    if (hi - lo < 1e-9) {
        hi += 1;
        lo -= 1;
    }
    double t0 = times.first(), span = times.last() - t0;
    if (span <= 0)
        return;
    QPainterPath path;
    bool pen = false;
    for (int i = 0; i < values.size(); ++i) {
        if (!std::isfinite(values[i])) {
            pen = false;
            continue;
        }
        QPointF point(box.x() + 8 + (times[i] - t0) / span * (box.width() - 16),
                      box.bottom() - 10 - (values[i] - lo) / (hi - lo) * (box.height() - 20));
        if (pen && i > 0 && times[i] - times[i - 1] < .5)
            path.lineTo(point);
        else
            path.moveTo(point);
        pen = true;
    }
    p.save();
    p.setClipRect(box);
    p.setPen(QPen(color, 1.2));
    p.drawPath(path);
    p.restore();
    p.setFont(QFont("Microsoft YaHei", 9));
    p.setPen(QColor("#526375"));
    p.drawText(QRectF(box.x(), box.bottom() + 5, box.width(), 20), QString("时间 %1—%2 秒   范围 %3—%4")
                                                                       .arg(t0, 0, 'f', 1)
                                                                       .arg(times.last(), 0, 'f', 1)
                                                                       .arg(lo, 0, 'f', 2)
                                                                       .arg(hi, 0, 'f', 2));
}
} // namespace
bool ReportWriter::verify(const QString &dir) {
    try {
        auto m = readJson(dir + "/manifest.json");
        auto files = m["files"].toObject();
        QStringList names{"report.pdf", "result.json", "signals.csv"};
        if (files.contains("emotions.jsonl")) names.append("emotions.jsonl");
        if (files.size() != names.size())
            return false;
        for (const auto &name : names)
            if (files[name].toString() != fileHash(dir + "/" + name))
                return false;
        return true;
    } catch (...) {
        return false;
    }
}
QString ReportWriter::generate(const Measurement &m) const {
    if (!QRegularExpression("^[a-zA-Z0-9-]{1,64}$").match(m.id).hasMatch() || m.status == "measuring")
        fail("报告需要有效的已结束测量。");
    QString dir = root_ + "/reports/" + m.id;
    QDir().mkpath(dir);
    if (QFileInfo::exists(dir + "/manifest.json")) {
        if (!verify(dir))
            fail("既有报告完整性校验失败，保留原文件供检查。");
        return dir + "/report.pdf";
    }
    writeFile(dir + "/result.json", QJsonDocument(m.json()).toJson());
    if (m.streamed) {
        copyFileChecked(root_ + "/signals/" + m.id + "/signals.csv", dir + "/signals.csv");
        copyFileChecked(root_ + "/signals/" + m.id + "/emotions.jsonl", dir + "/emotions.jsonl");
    } else {
    QByteArray csv = "time_seconds,red,green,blue,valid,break_continuity,quality_reason\n";
    for (auto s : m.samples)
        csv += QByteArray::number(s.t, 'f', 6) + "," + QByteArray::number(s.r, 'f', 6) + "," +
               QByteArray::number(s.g, 'f', 6) + "," + QByteArray::number(s.b, 'f', 6) + "," +
               (s.valid ? "1," : "0,") + (s.breakContinuity ? "1," : "0,") + s.qualityReason.toLatin1() + "\n";
    writeFile(dir + "/signals.csv", csv);
    }
    QString temporary = dir + "/report.pending.pdf";
    {
        QPdfWriter pdf(temporary);
        pdf.setPageSize(QPageSize(QPageSize::A4));
        pdf.setPageMargins(QMarginsF(0, 0, 0, 0));
        pdf.setResolution(96);
        pdf.setTitle("FaceVitals " + m.id);
        pdf.setCreator("FaceVitals C++ Desktop 0.6.0");
        QPainter p(&pdf);
        if (!p.isActive())
            fail("无法创建 PDF。");
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(pdf.width() / 800.0, pdf.height() / 1132.0);
        page(p, m, 1);
        paragraph(p, 140,
                  "受试者代号：" + m.subject + "\n采集时间（UTC）：" + m.created + "\n来源：" + m.source,
                  100);
        paragraph(p, 250, "完成状态：" + m.status + "\n原因：" + m.reason, 100);
        p.setFont(QFont("Microsoft YaHei", 28, QFont::Bold));
        p.setPen(QColor("#007f78"));
        p.drawText(QRectF(45, 370, 710, 55), "心率：" + textMetric(m.pulse.heartRate));
        if(m.diagnostics.contains("last_valid_heart_rate")) {
            const auto last=m.diagnostics["last_valid_heart_rate"].toObject();
            paragraph(p,422,QString("本次最近有效记录：%1 bpm（采集到 %2 秒时，不代表停止时状态）").arg(last["bpm"].toDouble(),0,'f',1).arg(last["at_seconds"].toDouble(),0,'f',1),24);
        }
        paragraph(p, 458,
                  "血压：未启用，缺少经验证的测量与校准流程。\n血氧：未启用，缺少经验证的光学与校准流程。\n呼"
                  "吸率 / PRV：尚未完成独立参考验证。",
                  110);
        int good = 0;
        for (auto s : m.samples)
            good += s.valid;
        const auto count = m.streamed ? m.totalSamples : m.samples.size();
        double coverage = !count ? 0 : 100.0 * (m.streamed ? m.validSamples : good) / count;
        paragraph(p, 580,
                  QString("采样点数：%1；有效点比例：%2%。\n分析窗质量分数：%"
                          "3（不是准确率）。\n心率使用末尾至多 30 秒片段，至少 20 秒；只允许有界短缺口，并检查覆盖率与频率稳定性。")
                      .arg(count)
                      .arg(coverage, 0, 'f', 1)
                      .arg(m.pulse.quality, 0, 'f', 3) + (m.streamed ? "\n图表显示最近两分钟；完整信号见 signals.csv 和 emotions.jsonl。" : ""),
                  110);
        paragraph(
            p, 710,
            "结论：" +
                (m.synthetic ? QString("此报告只验证软件流程。所有信号均为模拟，不能用于人体健康判断。")
                 : m.pulse.heartRate.status == "valid"
                     ? QString("获得了质量门槛内的摄像头心率研究估计。该数值尚未经过目标人群临床准确度验证。")
                     : QString("本次没有得到满足质量与时长要求的心率估计，请检查照明、姿态与设备。")),
            130);
        paragraph(
            p, 860,
            "表情模型版本：" + (m.modelId.isEmpty() ? QString("未加载") : m.modelId) +
                "\n算法：" + m.processing + "\n表情分析只描述模型对外部面部行为的判断，不能替代心理评估。",
            120);
        pdf.newPage();
        page(p, m, 2);
        plot(p, QRectF(45, 175, 710, 260), m.pulse.times, m.pulse.wave, "rPPG 脉搏波（归一化，非心电图）",
             QColor("#007f78"));
        QVector<double> ts, green;
        for (auto s : m.samples) {
            ts.append(s.t);
            green.append(s.valid ? s.g : std::numeric_limits<double>::quiet_NaN());
        }
        plot(p, QRectF(45, 530, 710, 200), ts, green, "绿色通道平均值（无效段断线）", QColor("#4164c1"));
        pdf.newPage();
        page(p, m, 3);
        if (m.emotions.isEmpty()) {
            paragraph(p, 160, "未生成表情曲线：未加载模型、未检测到单人脸部，或使用了模拟信号。", 100);
        } else {
            const auto labels = m.emotions.first().labels;
            const int count = std::min(16, int(labels.size()));
            const double rowHeight = 770.0 / std::max(1, count);
            for (int k = 0; k < count; ++k) {
                QVector<double> et, ev;
                for (auto e : m.emotions) {
                    et.append(e.t);
                    ev.append(e.scores.size() > k ? e.scores[k] : std::numeric_limits<double>::quiet_NaN());
                }
                plot(p, QRectF(45, 180 + k * rowHeight, 710, std::max(20.0, rowHeight - 48)), et, ev,
                     labels[k] + " · 模型分数（未校准）", QColor::fromHsv((k * 70) % 360, 170, 160));
            }
            paragraph(p, 985, m.streamed ? "完整分数保存在 emotions.jsonl；图表显示最近两分钟。" : "完整分数保存在 result.json；图表不是内心情绪的客观测量。", 55);
        }
        p.end();
    }
    QFile old(dir + "/report.pdf");
    if (old.exists() && !old.remove())
        fail("无法替换未完成的报告文件。");
    if (!QFile::rename(temporary, dir + "/report.pdf"))
        fail("PDF 原子提交失败。");
    QJsonObject files;
    QStringList names{"report.pdf", "result.json", "signals.csv"};
    if (m.streamed) names.append("emotions.jsonl");
    for (const auto &name : names)
        files[name] = fileHash(dir + "/" + name);
    writeFile(dir + "/manifest.json",
              QJsonDocument(
                  QJsonObject{{"schema", 1},
                              {"report_id", m.id},
                              {"files", files},
                              {"signed", false},
                              {"notice", "SHA-256 integrity only; not an authenticated digital signature"}})
                  .toJson());
    return dir + "/report.pdf";
}
} // namespace fv
