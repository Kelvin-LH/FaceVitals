#pragma once
#include "domain/Types.h"
#include <QFile>

namespace fv {
// 完整信号顺序落盘，内存只保留最近两分钟，供实时分析和报告预览使用。
class SessionArchive {
  public:
    void open(const QString &directory);
    void sample(Measurement &m, const Sample &value);
    void emotion(Measurement &m, const EmotionResult &value);
    void close();
  private:
    void write(QFile &file, const QByteArray &data);
    QFile signals_, emotions_;
    double lastFlush_ = 0;
};
}
