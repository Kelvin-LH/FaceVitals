#pragma once
#include "domain/Types.h"
#include <QThread>
#include <atomic>
#include <functional>

namespace fv {
class LocalTrainer final : public QThread {
    Q_OBJECT
  public:
    LocalTrainer(QString manifest, QString output, int epochs, QObject *parent = nullptr)
        : QThread(parent), manifest_(std::move(manifest)), output_(std::move(output)), epochs_(epochs) {}
    void cancel() {
        cancelled_ = true;
    }
    static QString train(const QString &manifest, const QString &output, int epochs,
                         std::atomic_bool &cancelled, const std::function<void(QString)> &progress);
  signals:
    void progress(QString text);
    void completed(QString directory, QString error);

  protected:
    void run() override;

  private:
    QString manifest_, output_;
    int epochs_;
    std::atomic_bool cancelled_{false};
};
} // namespace fv
