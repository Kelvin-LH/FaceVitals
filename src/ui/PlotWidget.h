#pragma once
#include <QVector>
#include <QWidget>
#include "domain/Types.h"

namespace fv {
class PlotWidget final : public QWidget {
  public:
    explicit PlotWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(270);
    }
    void setSignals(PulseResult pulse, QVector<Sample> samples) {
        pulse_ = std::move(pulse);
        samples_ = std::move(samples);
        update();
    }

  protected:
    void paintEvent(QPaintEvent *) override;

  private:
    PulseResult pulse_;
    QVector<Sample> samples_;
};
} // namespace fv
