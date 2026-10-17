#pragma once
#include "domain/Types.h"
#include <QWidget>

namespace fv {
class EmotionRadar final : public QWidget {
  public:
    explicit EmotionRadar(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(240, 240);
        setToolTip("表情模型分数（未经概率校准），不能用来推断真实心理状态。");
    }
    void setResult(EmotionResult result) { result_ = std::move(result); update(); }
  protected:
    void paintEvent(QPaintEvent *) override;
  private:
    EmotionResult result_;
};
}
