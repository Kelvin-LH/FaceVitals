#include "CameraFormats.h"
#include <cstring>

namespace fv {
QImage decodeCameraFrame(const uchar *data, qint64 bytes, int width, int height, int stride,
                         PixelFormat format, bool bt709, bool fullRange) {
    if (!data || bytes <= 0 || width < 1 || height < 1 || width > 8192 || height > 8192)
        fail("视频帧尺寸无效。");
    if (format == PixelFormat::Jpeg) {
        if (bytes > 128 * 1024 * 1024) fail("压缩视频帧过大。");
        return QImage::fromData(data, int(bytes), "JPEG").convertToFormat(QImage::Format_RGB888);
    }
    const bool packedYuv = format == PixelFormat::Yuyv || format == PixelFormat::Uyvy;
    const bool planarYuv = format == PixelFormat::Nv12 || format == PixelFormat::Yuv420;
    if ((packedYuv || planarYuv) && width % 2) fail("YUV 视频宽度必须为偶数。");
    if (planarYuv && (height % 2 || stride < 0)) fail("YUV 平面布局无效。");
    int channels = format == PixelFormat::Bgr32 ? 4 : (format == PixelFormat::Rgb24 || format == PixelFormat::Bgr24) ? 3 : packedYuv ? 2 : 1;
    if (!stride) stride = width * channels;
    const qint64 pitch = std::abs(qint64(stride));
    qint64 required = pitch * height;
    if (planarYuv) required += format == PixelFormat::Nv12 ? pitch * (height / 2) : 2 * ((pitch + 1) / 2) * (height / 2);
    if (pitch < width * channels || bytes < required) fail("视频帧长度或行步长无效。");
    QImage image(width, height, QImage::Format_RGB888);
    if (image.isNull()) fail("视频图像分配失败。");
    auto clip = [](int v) { return uchar(std::clamp(v, 0, 255)); };
    auto yuv = [&](uchar *pixel, int l, int u, int v) {
        u -= 128; v -= 128;
        const int y = fullRange ? 256 * l : 298 * (l - 16);
        const int rv = fullRange ? (bt709 ? 403 : 359) : (bt709 ? 459 : 409);
        const int gu = fullRange ? (bt709 ? 48 : 88) : (bt709 ? 55 : 100);
        const int gv = fullRange ? (bt709 ? 120 : 183) : (bt709 ? 136 : 208);
        const int bu = fullRange ? (bt709 ? 475 : 454) : (bt709 ? 541 : 516);
        pixel[0] = clip((y + rv * v + 128) >> 8);
        pixel[1] = clip((y - gu * u - gv * v + 128) >> 8);
        pixel[2] = clip((y + bu * u + 128) >> 8);
    };
    for (int y = 0; y < height; ++y) {
        const auto *row = data + (stride < 0 ? height - 1 - y : y) * pitch;
        auto *out = image.scanLine(y);
        for (int x = 0; x < width; ++x) {
            auto *pixel = out + x * 3;
            if (format == PixelFormat::Rgb24) std::memcpy(pixel, row + x * 3, 3);
            else if (format == PixelFormat::Bgr24 || format == PixelFormat::Bgr32) {
                pixel[0] = row[x * channels + 2]; pixel[1] = row[x * channels + 1]; pixel[2] = row[x * channels];
            } else if (format == PixelFormat::Gray) pixel[0] = pixel[1] = pixel[2] = row[x];
            else if (packedYuv) {
                const auto *pair = row + (x / 2) * 4;
                if (format == PixelFormat::Yuyv) yuv(pixel, pair[(x % 2) * 2], pair[1], pair[3]);
                else yuv(pixel, pair[1 + (x % 2) * 2], pair[0], pair[2]);
            } else if (format == PixelFormat::Nv12) {
                const auto *uv = data + pitch * height + (y / 2) * pitch + (x / 2) * 2;
                yuv(pixel, row[x], uv[0], uv[1]);
            } else {
                const qint64 cp = (pitch + 1) / 2;
                const auto *uv = data + pitch * height;
                yuv(pixel, row[x], uv[(y / 2) * cp + x / 2], uv[cp * (height / 2) + (y / 2) * cp + x / 2]);
            }
        }
    }
    return image;
}
}
