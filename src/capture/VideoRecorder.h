#pragma once
#include <QFile>
#include <QImage>
#include <QVector>

namespace fv {
// 跨平台 MJPEG/AVI 写入器；固定 10 fps，按时间戳补帧，不采集音频。
class VideoRecorder {
  public:
    void open(const QString &path, const QImage &frame, double time);
    void append(const QImage &frame, double time);
    void close();
    bool isOpen() const { return file_.isOpen(); }
    int frames() const { return index_.size(); }
    qint64 bytesWritten() const { return file_.pos(); }
    static constexpr int fps = 10;
  private:
    void write(const QByteArray &data);
    void patch(qint64 position, quint32 value);
    void chunk(const QByteArray &jpeg);
    QFile file_;
    QString finalPath_;
    QSize size_;
    double start_ = 0;
    qint64 totalAt_ = 0, lengthAt_ = 0, moviAt_ = 0;
    QVector<QPair<quint32, quint32>> index_;
    QByteArray previous_;
};
}
