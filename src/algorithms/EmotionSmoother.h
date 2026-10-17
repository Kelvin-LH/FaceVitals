#pragma once
#include "domain/Types.h"
#include <algorithm>
#include <cmath>

namespace fv {
// 时间相关指数平滑；离脸、时间跳变与类别变化时丢弃旧状态。
class EmotionSmoother final {
  public:
    void reset() { previous_ = {}; }
    EmotionResult update(EmotionResult current) {
        if (current.scores.isEmpty()) { reset(); return current; }
        // 图像质量已拒绝的帧不能被历史高分重新变成有效表情。
        if (current.status == "poor_image") { reset(); return current; }
        double sum = 0;
        for (double score : current.scores) {
            if (!std::isfinite(score) || score < 0) {
                reset(); current.scores.clear(); current.status = "invalid_output";
                current.label = "无法判断"; return current;
            }
            sum += score;
        }
        if (current.labels.size() != current.scores.size() || !std::isfinite(current.t) ||
            !std::isfinite(sum) || sum <= 0) {
            reset(); current.scores.clear(); current.status = "invalid_output";
            current.label = "无法判断"; return current;
        }
        for (double &score : current.scores) score /= sum;
        const double dt = current.t - previous_.t;
        if (previous_.labels == current.labels && previous_.scores.size() == current.scores.size() &&
            dt > 0 && dt < 1.0) {
            const double alpha = 1 - std::exp(-dt / .45);
            for (int i = 0; i < current.scores.size(); ++i)
                current.scores[i] = alpha * current.scores[i] + (1 - alpha) * previous_.scores[i];
        }
        auto sorted = current.scores;
        std::sort(sorted.begin(), sorted.end(), std::greater<double>());
        const auto best = std::max_element(current.scores.begin(), current.scores.end());
        const bool confident = *best >= .5 && (sorted.size() < 2 || sorted[0] - sorted[1] >= .12);
        current.status = confident ? "valid" : "uncertain";
        current.label = confident ? current.labels[int(best - current.scores.begin())] : "无法判断";
        previous_ = current;
        return current;
    }
  private:
    EmotionResult previous_;
};
}
