#include "VideoRecorder.h"
#include "domain/Types.h"
#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QtEndian>
#include <cmath>

namespace fv {
namespace {
QByteArray u32(quint32 v) { QByteArray b(4, 0); qToLittleEndian(v, b.data()); return b; }
QByteArray block(const char *id, QByteArray b) { return QByteArray(id, 4) + u32(b.size()) + b; }
void put(QByteArray &b, int at, quint32 v) { qToLittleEndian(v, b.data() + at); }
}
void VideoRecorder::write(const QByteArray &data) {
    if (file_.write(data) != data.size()) fail("录像写入失败，请检查磁盘空间。");
}
void VideoRecorder::patch(qint64 position, quint32 value) {
    if (!file_.seek(position)) fail("录像索引写入失败。");
    write(u32(value));
}
void VideoRecorder::open(const QString &path, const QImage &frame, double time) {
    if (isOpen() || frame.isNull()) fail("录像状态无效。");
    QDir().mkpath(QFileInfo(path).absolutePath());
    finalPath_ = path;
    file_.setFileName(path + ".partial");
    if (QFileInfo::exists(path) || !file_.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        fail("无法创建录像文件。");
    size_ = frame.size(); start_ = time; index_.clear(); previous_.clear();
    QByteArray avih(56, 0), strh(56, 0), strf(40, 0);
    put(avih, 0, 1000000 / fps); put(avih, 12, 0x10); put(avih, 24, 1);
    put(avih, 32, size_.width()); put(avih, 36, size_.height());
    strh.replace(0, 8, "vidsMJPG"); put(strh, 20, 1); put(strh, 24, fps); put(strh, 40, 0xffffffff);
    qToLittleEndian<quint16>(quint16(size_.width()), strh.data() + 52);
    qToLittleEndian<quint16>(quint16(size_.height()), strh.data() + 54);
    put(strf, 0, 40); put(strf, 4, size_.width()); put(strf, 8, size_.height());
    qToLittleEndian<quint16>(1, strf.data() + 12); qToLittleEndian<quint16>(24, strf.data() + 14);
    strf.replace(16, 4, "MJPG"); put(strf, 20, size_.width() * size_.height() * 3);
    QByteArray header = "RIFF" + u32(0) + "AVI " + block("LIST", "hdrl" + block("avih", avih) +
        block("LIST", "strl" + block("strh", strh) + block("strf", strf)));
    totalAt_ = header.indexOf("avih") + 8 + 16;
    lengthAt_ = header.indexOf("strh") + 8 + 32;
    write(header); moviAt_ = file_.pos(); write("LIST" + u32(0) + "movi");
    append(frame, time);
}
void VideoRecorder::chunk(const QByteArray &jpeg) {
    if (file_.pos() + jpeg.size() > 1024LL * 1024 * 1024) fail("单段录像已达到 1 GiB 上限。");
    index_.append({quint32(file_.pos() - moviAt_ - 8), quint32(jpeg.size())});
    write(block("00dc", jpeg));
    if (jpeg.size() % 2) write(QByteArray(1, 0));
}
void VideoRecorder::append(const QImage &frame, double time) {
    if (!isOpen()) return;
    int target = int(std::floor((time - start_) * fps));
    if (target < index_.size()) return;
    // 缺失的时刻重复上一帧，避免较低采集帧率导致录像加速。
    while (!previous_.isEmpty() && index_.size() < target) chunk(previous_);
    QByteArray jpeg; QBuffer buffer(&jpeg); buffer.open(QIODevice::WriteOnly);
    auto fixed = frame.size() == size_ ? frame : frame.scaled(size_);
    if (!fixed.save(&buffer, "JPEG", 80)) fail("JPEG 编码失败。");
    chunk(jpeg); previous_ = jpeg;
}
void VideoRecorder::close() {
    if (!isOpen()) return;
    const qint64 end = file_.pos();
    QByteArray index;
    for (auto entry : index_) index += QByteArray("00dc") + u32(0x10) + u32(entry.first) + u32(entry.second);
    write(block("idx1", index));
    const qint64 size = file_.pos();
    patch(4, quint32(size - 8)); patch(totalAt_, frames()); patch(lengthAt_, frames());
    patch(moviAt_ + 4, quint32(end - moviAt_ - 8));
    if (!file_.flush()) fail("录像保存失败。");
    file_.close();
    if (!file_.rename(finalPath_)) fail("录像文件完成后重命名失败。");
}
}
