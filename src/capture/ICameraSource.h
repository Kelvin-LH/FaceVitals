#pragma once
#include "domain/Types.h"
#include <memory>

namespace fv {
struct CameraInfo {
    QString id, name;
};
struct CameraOptions {
    int width = 1280, height = 720, fps = 30;
};
class ICameraSource {
  public:
    virtual ~ICameraSource() = default;
    virtual void open(const QString &id) = 0;
    // 每次最多等待约 100ms；空图表示暂时无帧，停止请求可及时响应。
    virtual QImage read() = 0;
    virtual QString description() const { return {}; }
};
QVector<CameraInfo> enumerateCameras();
std::unique_ptr<ICameraSource> createCamera(const CameraOptions &options = {});
} // namespace fv
