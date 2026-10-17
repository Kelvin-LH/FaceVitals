#include "ICameraSource.h"
#include "CameraFormats.h"
#include <QFileInfo>
#include <QDir>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace fv {
namespace {
int control(int fd, unsigned long op, void *data) {
    int r;
    do {
        r = ioctl(fd, op, data);
    } while (r < 0 && errno == EINTR);
    return r;
}
class LinuxCamera final : public ICameraSource {
  public:
    explicit LinuxCamera(CameraOptions options) : options_(options) {}
    ~LinuxCamera() override {
        if (fd_ >= 0) {
            auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (streaming_)
                control(fd_, VIDIOC_STREAMOFF, &type);
            for (auto b : buffers_)
                if (b.first != MAP_FAILED)
                    munmap(b.first, b.second);
            ::close(fd_);
        }
    }
    void open(const QString &id) override {
        if (!(id.startsWith("/dev/video") || id.startsWith("/dev/v4l/by-id/")) || id.contains(".."))
            fail("摄像头路径无效。");
        fd_ = ::open(id.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
        if (fd_ < 0)
            fail("无法打开摄像头，请检查 video 组权限。");
        v4l2_capability cap{};
        if (control(fd_, VIDIOC_QUERYCAP, &cap) < 0) fail("设备不是 V4L2 摄像头。");
        const unsigned caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
        if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) fail("设备不支持单平面视频采集。");
        bool selected = false;
        for (auto size : {QSize(options_.width, options_.height), QSize(1280,720), QSize(640,480), QSize(1920,1080)}) {
            for (unsigned fmt : {V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_RGB24,
                                 V4L2_PIX_FMT_BGR24, V4L2_PIX_FMT_UYVY, V4L2_PIX_FMT_YUV420}) {
                v4l2_format f{}; f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                f.fmt.pix.width = size.width(); f.fmt.pix.height = size.height();
                f.fmt.pix.pixelformat = fmt; f.fmt.pix.field = V4L2_FIELD_ANY;
                if (control(fd_, VIDIOC_S_FMT, &f) < 0) continue;
                switch (f.fmt.pix.pixelformat) {
                case V4L2_PIX_FMT_MJPEG: pixel_ = PixelFormat::Jpeg; break;
                case V4L2_PIX_FMT_YUYV: pixel_ = PixelFormat::Yuyv; break;
                case V4L2_PIX_FMT_UYVY: pixel_ = PixelFormat::Uyvy; break;
                case V4L2_PIX_FMT_NV12: pixel_ = PixelFormat::Nv12; break;
                case V4L2_PIX_FMT_YUV420: pixel_ = PixelFormat::Yuv420; break;
                case V4L2_PIX_FMT_RGB24: pixel_ = PixelFormat::Rgb24; break;
                case V4L2_PIX_FMT_BGR24: pixel_ = PixelFormat::Bgr24; break;
                default: continue;
                }
                width_ = f.fmt.pix.width; height_ = f.fmt.pix.height; stride_ = f.fmt.pix.bytesperline;
                if (width_ < 32 || height_ < 32 || width_ > 4096 || height_ > 4096) continue;
                format_ = f.fmt.pix.pixelformat;
                bt709_ = f.fmt.pix.ycbcr_enc == V4L2_YCBCR_ENC_709 ||
                    (f.fmt.pix.ycbcr_enc == V4L2_YCBCR_ENC_DEFAULT && f.fmt.pix.colorspace == V4L2_COLORSPACE_REC709);
                full_ = f.fmt.pix.quantization == V4L2_QUANTIZATION_FULL_RANGE ||
                    (f.fmt.pix.quantization == V4L2_QUANTIZATION_DEFAULT && f.fmt.pix.colorspace == V4L2_COLORSPACE_JPEG);
                selected = true; break;
            }
            if (selected) break;
        }
        if (!selected) fail("摄像头没有可解码的视频模式。");
        v4l2_streamparm p{}; p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        p.parm.capture.timeperframe = {1, static_cast<unsigned>(options_.fps)};
        control(fd_, VIDIOC_S_PARM, &p);
        if (p.parm.capture.timeperframe.numerator) fps_ = double(p.parm.capture.timeperframe.denominator) / p.parm.capture.timeperframe.numerator;
        v4l2_requestbuffers req{};
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        req.count = 4;
        if (control(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2)
            fail("无法创建相机缓冲区。");
        for (unsigned int i = 0; i < req.count; ++i) {
            v4l2_buffer b{};
            b.type = req.type;
            b.memory = req.memory;
            b.index = i;
            if (control(fd_, VIDIOC_QUERYBUF, &b) < 0)
                fail("相机缓冲区查询失败。");
            void *mem = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, b.m.offset);
            if (mem == MAP_FAILED)
                fail("相机内存映射失败。");
            buffers_.append({mem, b.length});
            if (control(fd_, VIDIOC_QBUF, &b) < 0)
                fail("相机缓冲区入队失败。");
        }
        auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (control(fd_, VIDIOC_STREAMON, &type) < 0)
            fail("摄像头启动失败。");
        streaming_ = true;
    }
    QImage read() override {
        pollfd p{fd_, POLLIN, 0};
        int n = poll(&p, 1, 100);
        if (n == 0 || (n < 0 && errno == EINTR))
            return {};
        if (n < 0 || (p.revents & (POLLERR | POLLHUP)))
            fail("摄像头已断开。");
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (control(fd_, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN)
                return {};
            fail("摄像头读取失败。");
        }
        if (b.index >= unsigned(buffers_.size()) || b.bytesused > buffers_[int(b.index)].second)
            fail("视频缓冲区长度无效。");
        auto *data = static_cast<const uchar *>(buffers_[int(b.index)].first);
        const auto image = decodeCameraFrame(data, b.bytesused, width_, height_, stride_, pixel_, bt709_, full_);
        if (control(fd_, VIDIOC_QBUF, &b) < 0)
            fail("视频重新排队失败。");
        return image;
    }

    QString description() const override {
        char fourcc[5] = {char(format_), char(format_ >> 8), char(format_ >> 16), char(format_ >> 24), 0};
        return QString("V4L2 · %1×%2 · %3 fps · %4").arg(width_).arg(height_).arg(fps_, 0, 'f', 1).arg(fourcc);
    }
  private:
    CameraOptions options_;
    PixelFormat pixel_ = PixelFormat::Jpeg;
    bool bt709_ = false, full_ = false;
    double fps_ = 0;
    int fd_ = -1, width_ = 0, height_ = 0, stride_ = 0;
    unsigned int format_ = 0;
    bool streaming_ = false;
    QVector<QPair<void *, size_t>> buffers_;
};
} // namespace
QVector<CameraInfo> enumerateCameras() {
    QVector<CameraInfo> result;
    for (auto file : QDir("/dev").entryList({"video*"}, QDir::System | QDir::Files)) {
        QString path = "/dev/" + file;
        int fd = ::open(path.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
        if (fd < 0)
            continue;
        v4l2_capability cap{};
        if (control(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            unsigned capabilities =
                cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
            if ((capabilities & V4L2_CAP_VIDEO_CAPTURE) && (capabilities & V4L2_CAP_STREAMING))
                {
                QString stable = path;
                for (const auto &entry : QDir("/dev/v4l/by-id").entryInfoList(QDir::Files | QDir::System))
                    if (entry.canonicalFilePath() == path) { stable = entry.absoluteFilePath(); break; }
                result.append({stable, QString::fromUtf8(reinterpret_cast<char *>(cap.card))});
            }
        }
        ::close(fd);
    }
    return result;
}
std::unique_ptr<ICameraSource> createCamera(const CameraOptions &options) {
    return std::make_unique<LinuxCamera>(options);
}
} // namespace fv
