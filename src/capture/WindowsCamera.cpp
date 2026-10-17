#include "ICameraSource.h"
#include "CameraFormats.h"
#include <Windows.h>
#include <atomic>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mutex>
#include <wrl/client.h>
#include <algorithm>
#include <QJsonDocument>

namespace fv {
using Microsoft::WRL::ComPtr;
QVector<CameraInfo> enumerateDirectShowCameras();
std::unique_ptr<ICameraSource> createDirectShowCamera(const CameraOptions &options);
namespace {
constexpr DWORD videoStream = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
QString deviceKey(QString id) {
    if (id.startsWith("mf:")) id.remove(0,3);
    if (id.startsWith("dshow:")) id.remove(0,6);
    const int category = id.indexOf("#{");
    return (category >= 0 ? id.left(category) : id).toLower();
}
QString attributeString(IMFAttributes *attributes, const GUID &key) {
    WCHAR *text = nullptr; UINT32 size = 0;
    attributes->GetAllocatedString(key, &text, &size);
    const auto value = text ? QString::fromWCharArray(text) : QString();
    CoTaskMemFree(text); return value;
}
void check(HRESULT hr, const char *message) {
    if (FAILED(hr))
        fail(QString::fromUtf8(message) + QString(" (0x%1)").arg(quint32(hr), 8, 16, QChar('0')));
}
class MfRuntime {
  public:
    MfRuntime() {
        hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr_) && hr_ != RPC_E_CHANGED_MODE)
            check(hr_, "COM 初始化失败");
        check(MFStartup(MF_VERSION), "Media Foundation 初始化失败");
    }
    ~MfRuntime() {
        MFShutdown();
        if (SUCCEEDED(hr_))
            CoUninitialize();
    }

  private:
    HRESULT hr_;
};
class Devices {
  public:
    Devices() {
        ComPtr<IMFAttributes> attrs;
        check(MFCreateAttributes(&attrs, 1), "创建设备参数失败");
        attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        check(MFEnumDeviceSources(attrs.Get(), &items, &count), "摄像头枚举失败");
    }
    ~Devices() {
        for (UINT32 i = 0; i < count; ++i)
            items[i]->Release();
        CoTaskMemFree(items);
    }
    IMFActivate **items = nullptr;
    UINT32 count = 0;
};
class ReaderCallback final : public IMFSourceReaderCallback {
  public:
    ReaderCallback() {
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) fail("无法创建摄像头事件。");
    }
    ~ReaderCallback() {
        CloseHandle(event);
    }
    STDMETHODIMP QueryInterface(REFIID iid, void **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback)) {
            *out = static_cast<IMFSourceReaderCallback *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return ++refs;
    }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG n = --refs;
        if (!n)
            delete this;
        return n;
    }
    STDMETHODIMP OnReadSample(HRESULT hr, DWORD, DWORD flags, LONGLONG, IMFSample *value) override {
        {
            std::lock_guard<std::mutex> guard(lock);
            status = hr;
            sample = value;
            end = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
            typeChanged = (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0;
        }
        SetEvent(event);
        return S_OK;
    }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent *) override {
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD) override {
        SetEvent(event);
        return S_OK;
    }
    std::mutex lock;
    ComPtr<IMFSample> sample;
    HANDLE event;
    HRESULT status = S_OK;
    bool end = false;
    bool typeChanged = false;

  private:
    std::atomic<ULONG> refs{1};
};
class WindowsCamera final : public ICameraSource {
  public:
    explicit WindowsCamera(CameraOptions options) : options_(options) {}
    ~WindowsCamera() override {
        if (reader_)
            reader_->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        reader_.Reset();
        if (source_)
            source_->Shutdown();
        source_.Reset();
        callback_.Reset();
    }
    void open(const QString &id) override {
        Devices devices;
        bool ok = false;
        int index = id.toInt(&ok);
        if (!ok) {
            index = -1;
            for (UINT32 i = 0; i < devices.count; ++i)
                if (attributeString(devices.items[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK).compare(id, Qt::CaseInsensitive) == 0)
                    index = int(i);
            ok = index >= 0;
        }
        if (!ok || index < 0 || index >= int(devices.count))
            fail("摄像头不存在。");
        check(devices.items[index]->ActivateObject(IID_PPV_ARGS(&source_)),
              "无法打开摄像头，请检查隐私权限或占用");
        callback_.Attach(new ReaderCallback());
        ComPtr<IMFAttributes> attrs;
        check(MFCreateAttributes(&attrs, 3), "创建采集参数失败");
        attrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback_.Get());
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        auto createResult = MFCreateSourceReaderFromMediaSource(source_.Get(), attrs.Get(), &reader_);
        if (FAILED(createResult)) {
            attrs->DeleteItem(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING);
            attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
            createResult = MFCreateSourceReaderFromMediaSource(source_.Get(), attrs.Get(), &reader_);
        }
        check(createResult, "创建采集器失败");
        // 逐个尝试原生模式，输出类型携带分辨率与帧率，防止转换时退回设备默认 2K/4K。
        QVector<QPair<double, ComPtr<IMFMediaType>>> candidates;
        QJsonArray modeTrace;
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> type;
            HRESULT hr = reader_->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &type);
            if (FAILED(hr))
                break;
            UINT32 w = 0, h = 0, num = 0, den = 1;
            MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
            MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den);
            double fps = den ? double(num) / den : 0;
            modeTrace.append(QJsonObject{{"width",int(w)},{"height",int(h)},{"fps",fps}});
            if (w < 160 || h < 120 || w > 4096 || h > 4096 || fps > 120)
                continue;
            candidates.append({cameraModeScore(int(w), int(h), fps, options_), type});
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        if (!qEnvironmentVariable("FV_CAMERA_MODES").isEmpty()) writeFile(qEnvironmentVariable("FV_CAMERA_MODES"),QJsonDocument(modeTrace).toJson());
        HRESULT result = E_FAIL;
        for (const auto &candidate : candidates) {
            ComPtr<IMFSourceReaderEx> extended;
            DWORD flags = 0;
            if (SUCCEEDED(reader_.As(&extended))) {
                if (FAILED(extended->SetNativeMediaType(videoStream, candidate.second.Get(), &flags))) continue;
            } else if (FAILED(reader_->SetCurrentMediaType(videoStream, nullptr, candidate.second.Get()))) continue;
            ComPtr<IMFMediaType> rgb;
            check(MFCreateMediaType(&rgb), "创建视频类型失败");
            rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            UINT64 value = 0;
            if (SUCCEEDED(candidate.second->GetUINT64(MF_MT_FRAME_SIZE, &value))) rgb->SetUINT64(MF_MT_FRAME_SIZE, value);
            if (SUCCEEDED(candidate.second->GetUINT64(MF_MT_FRAME_RATE, &value))) rgb->SetUINT64(MF_MT_FRAME_RATE, value);
            result = reader_->SetCurrentMediaType(videoStream, nullptr, rgb.Get());
            if (SUCCEEDED(result)) break;
        }
        check(result, "没有可用的相机采集模式");
        readFormat();
    }
    QString description() const override {
        return QString("Media Foundation · %1×%2 · %3 fps · RGB32").arg(width_).arg(height_).arg(fps_, 0, 'f', 1);
    }
    void readFormat() {
        ComPtr<IMFMediaType> actual;
        check(reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual), "读取相机模式失败");
        check(MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &width_, &height_), "读取相机尺寸失败");
        if (width_ < 1 || height_ < 1 || width_ > 8192 || height_ > 8192) fail("相机分辨率无效。");
        UINT32 numerator = 0, denominator = 1;
        if (SUCCEEDED(MFGetAttributeRatio(actual.Get(), MF_MT_FRAME_RATE, &numerator, &denominator)) && denominator)
            fps_ = double(numerator) / denominator;
        UINT32 rawStride = 0;
        if (SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride)))
            stride_ = static_cast<LONG>(rawStride);
        else
            stride_ = LONG(width_ * 4);
    }
    QImage read() override {
        if (!pending_) {
            check(reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr,
                                      nullptr),
                  "读取摄像头失败");
            pending_ = true;
        }
        if (WaitForSingleObject(callback_->event, 100) != WAIT_OBJECT_0)
            return {};
        pending_ = false;
        ComPtr<IMFSample> sample;
        {
            std::lock_guard<std::mutex> guard(callback_->lock);
            check(callback_->status, "摄像头断开或读取失败");
            if (callback_->end)
                fail("摄像头已停止。");
            sample = callback_->sample;
            callback_->sample.Reset();
            if (callback_->typeChanged) readFormat();
        }
        if (!sample)
            return {};
        ComPtr<IMFMediaBuffer> buffer;
        check(sample->ConvertToContiguousBuffer(&buffer), "视频缓冲区错误");
        ComPtr<IMF2DBuffer> buffer2d;
        if (SUCCEEDED(buffer.As(&buffer2d))) {
            BYTE *data = nullptr;
            LONG stride = 0;
            check(buffer2d->Lock2D(&data, &stride), "锁定视频失败");
            if (std::abs(qint64(stride)) < width_ * 4) { buffer2d->Unlock2D(); fail("相机行步长不足。"); }
            QImage frame(int(width_), int(height_), QImage::Format_RGB32);
            for (UINT32 y = 0; y < height_; ++y)
                memcpy(frame.scanLine(int(y)), data + ptrdiff_t(y) * stride, width_ * 4);
            buffer2d->Unlock2D();
            return frame.convertToFormat(QImage::Format_RGB888);
        }
        BYTE *data = nullptr;
        DWORD length = 0;
        check(buffer->Lock(&data, nullptr, &length), "锁定视频失败");
        QImage frame(int(width_), int(height_), QImage::Format_RGB32);
        LONG stride = stride_;
        if (std::abs(qint64(stride)) < width_ * 4 || length < std::abs(qint64(stride)) * height_) {
            buffer->Unlock();
            fail("视频帧长度不足。");
        }
        const BYTE *first = stride < 0 ? data + (height_ - 1) * size_t(-stride) : data;
        for (UINT32 y = 0; y < height_; ++y)
            memcpy(frame.scanLine(int(y)), first + ptrdiff_t(y) * stride, width_ * 4);
        buffer->Unlock();
        return frame.convertToFormat(QImage::Format_RGB888);
    }

  private:
    MfRuntime runtime_;
    ComPtr<IMFMediaSource> source_;
    ComPtr<IMFSourceReader> reader_;
    ComPtr<ReaderCallback> callback_;
    UINT32 width_ = 0, height_ = 0;
    LONG stride_ = 0;
    bool pending_ = false;
    CameraOptions options_;
    double fps_ = 0;
};
class WindowsCameraRouter final : public ICameraSource {
  public:
    explicit WindowsCameraRouter(CameraOptions options) : options_(options) {}
    void open(const QString &id) override {
        if (id.startsWith("dshow:")) {
            camera_ = createDirectShowCamera(options_); camera_->open(id.mid(6)); return;
        }
        const auto device = id.startsWith("mf:") ? id.mid(3) : id;
        try { camera_ = std::make_unique<WindowsCamera>(options_); camera_->open(device); }
        catch (const std::exception &mfError) {
            const auto reason = QString::fromUtf8(mfError.what()); camera_.reset();
            QString fallback = device;
            for (const auto &c : enumerateDirectShowCameras()) if (deviceKey(c.id) == deviceKey(device)) { fallback = c.id.mid(6); break; }
            try { camera_ = createDirectShowCamera(options_); camera_->open(fallback); }
            catch (const std::exception &dsError) { fail(reason + "；兼容通道：" + QString::fromUtf8(dsError.what())); }
        }
    }
    QImage read() override { return camera_->read(); }
    QString description() const override { return camera_ ? camera_->description() : QString(); }
  private:
    CameraOptions options_;
    std::unique_ptr<ICameraSource> camera_;
};
} // namespace
QVector<CameraInfo> enumerateCameras() {
    MfRuntime runtime;
    Devices devices;
    QVector<CameraInfo> list;
    for (UINT32 i = 0; i < devices.count; ++i) {
        const auto id = attributeString(devices.items[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
        const auto name = attributeString(devices.items[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        if (!id.isEmpty()) list.append({"mf:" + id, name.isEmpty() ? QString("Camera %1").arg(i) : name});
    }
    for (const auto &device : enumerateDirectShowCameras()) {
        bool duplicate = false;
        for (const auto &mf : list) if (deviceKey(mf.id) == deviceKey(device.id)) duplicate = true;
        if (!duplicate) list.append(device);
    }
    return list;
}
std::unique_ptr<ICameraSource> createCamera(const CameraOptions &options) {
    return std::make_unique<WindowsCameraRouter>(options);
}
} // namespace fv
