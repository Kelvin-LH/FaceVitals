#pragma once
#include "domain/Types.h"

namespace fv {
class ReportWriter final {
  public:
    explicit ReportWriter(QString root) : root_(std::move(root)) {}
    QString generate(const Measurement &measurement) const;
    static bool verify(const QString &directory);

  private:
    QString root_;
};
} // namespace fv
