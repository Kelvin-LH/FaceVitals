#pragma once
#include "domain/Types.h"

namespace fv {
struct RoiSignal {
    Sample sample;
    int regionMask = 0;
    double retainedFraction = 0;
};
// 在额头与双颊中排除过曝/过暗像素；至少两个区域可用才输出颜色信号。
RoiSignal extractRoiSignal(const QImage &frame, const QRect &face, double time, int requiredMask = 0);
}
