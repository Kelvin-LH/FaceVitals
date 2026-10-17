#pragma once
#include "domain/Types.h"

namespace fv {
// 暂留值保留真实观测时刻，并使用独立状态；不会增加算法有效输出计数。
inline PulseResult pulseForDisplay(PulseResult current, const PulseResult &lastValid, double now) {
    if (current.heartRate.status != "valid" && lastValid.heartRate.status == "valid" &&
        now >= lastValid.measuredAt && now-lastValid.measuredAt <= 3.0) {
        current.heartRate = lastValid.heartRate;
        current.heartRate.status = "recent";
        current.heartRate.reason = "最近一次有效估计；当前正在重新检查信号";
        current.measuredAt = lastValid.measuredAt;
    }
    return current;
}
}
