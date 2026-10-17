#pragma once
#include "ICameraSource.h"
#include <algorithm>
#include <cmath>

namespace fv {
inline double cameraModeScore(int width, int height, double fps, const CameraOptions &options) {
    return (fps > 0 && fps < 15 ? 100000 : 0) + std::abs((fps > 0 ? fps : 30) - options.fps) * 500 +
        std::abs(width - options.width) + std::abs(height - options.height);
}
enum class PixelFormat { Rgb24, Bgr24, Bgr32, Yuyv, Uyvy, Nv12, Yuv420, Gray, Jpeg };
// stride 可带行填充；负 stride 的缓冲区按自底向上排列。YUV 支持 BT.601 / BT.709。
QImage decodeCameraFrame(const uchar *data, qint64 bytes, int width, int height, int stride,
                         PixelFormat format, bool bt709 = false, bool fullRange = false);
}
