#pragma once
#include "domain/Types.h"
#include "ICameraSource.h"
#include <functional>
#include <QMutex>
#include <QThread>
#include <atomic>

namespace fv {
class CaptureWorker final : public QThread {
    Q_OBJECT
  public:
    CaptureWorker(Measurement measurement, QString camera, QString detector, QString model, int duration,
                  QObject *parent = nullptr);
    void cancel() {
        cancelled_ = true;
    }
    Preview latest();
    void configureCamera(CameraOptions options) { cameraOptions_ = options; }
    void setCameraFactory(std::function<std::unique_ptr<ICameraSource>()> factory) { cameraFactory_ = std::move(factory); }
    void configureRecording(const QString &directory, bool enabled, int segmentSeconds = 600) { recordingDirectory_ = directory; recording_ = enabled; segmentSeconds_ = std::max(1, segmentSeconds); }
    void configureArchive(const QString &directory) { archiveDirectory_ = directory; }
    void setRecording(bool enabled) { recording_ = enabled; }
  signals:
    void completed(fv::Measurement measurement);

  protected:
    void run() override;

  private:
    Measurement measurement_;
    QString camera_, detector_, model_;
    int duration_;
    CameraOptions cameraOptions_;
    std::function<std::unique_ptr<ICameraSource>()> cameraFactory_;
    std::atomic_bool cancelled_{false};
    std::atomic_bool recording_{false};
    QString recordingDirectory_;
    QString archiveDirectory_;
    int segmentSeconds_ = 600;
    QMutex mutex_;
    Preview preview_;
};
} // namespace fv
