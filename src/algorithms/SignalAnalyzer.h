#pragma once
#include "domain/Types.h"

namespace fv {
class SignalAnalyzer final {
  public:
    PulseResult analyze(const QVector<Sample> &input) const;
};
} // namespace fv
