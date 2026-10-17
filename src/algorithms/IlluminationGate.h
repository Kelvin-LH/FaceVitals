#pragma once
#include "domain/Types.h"
#include <cmath>

namespace fv {
// 共同亮度缩放可由归一化抵消；大幅曝光变化与色度突变仍切断连续片段。
class IlluminationGate {
  public:
    void reset() { mean_ = -1; majorChange_ = false; }
    bool majorChange() const { return majorChange_; }
    bool accept(const Sample &s) {
        const double mean = (s.r + s.g + s.b) / 3;
        if (!std::isfinite(mean) || mean <= 0) { reset(); return false; }
        const double red = s.r / mean, blue = s.b / mean;
        majorChange_ = mean_ >= 0 && (std::abs(mean-mean_)/mean_>.35 ||
            std::abs(red-red_)>.10 || std::abs(blue-blue_)>.10);
        const bool stable = mean_ < 0 || (std::abs(mean - mean_) / mean_ <= .20 &&
            std::abs(red - red_) <= .025 && std::abs(blue - blue_) <= .025);
        mean_ = mean; red_ = red; blue_ = blue;
        return stable;
    }
  private:
    double mean_ = -1, red_ = 0, blue_ = 0;
    bool majorChange_ = false;
};
}
