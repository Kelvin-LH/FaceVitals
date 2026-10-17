#include "ICameraSource.h"
#include "CameraFormats.h"
#include <Windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <dxva.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <cstring>

namespace fv {
using Microsoft::WRL::ComPtr;
namespace {
// Sample Grabber 是 Windows 自带组件；现代 SDK 不再提供 qedit.h，按公开 COM 接口声明。
MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85") GrabberCallback : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample *) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE *, long) = 0;
};
MIDL_INTERFACE("6B652FFF-11FE-4FCE-92AD-0266B5D7C78F") SampleGrabber : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long *, long *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(GrabberCallback *, long) = 0;
};
const GUID grabberClass{0xC1F400A0, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
const GUID nullRendererClass{0xC1F400A4, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
void checkDs(HRESULT hr, const char *message) {
    if (FAILED(hr)) fail(QString::fromUtf8(message) + QString(" (0x%1)").arg(quint32(hr), 8, 16, QChar('0')));
}
class ComRuntime {
  public:
    ComRuntime() : status_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {
        if (status_ != RPC_E_CHANGED_MODE) checkDs(status_, "DirectShow 初始化失败");
    }
    ~ComRuntime() { if (SUCCEEDED(status_)) CoUninitialize(); }
  private: HRESULT status_;
};
void clearType(AM_MEDIA_TYPE &type) {
    CoTaskMemFree(type.pbFormat);
    if (type.pUnk) type.pUnk->Release();
    type = {};
}
BITMAPINFOHEADER *bitmap(AM_MEDIA_TYPE &type, REFERENCE_TIME **interval = nullptr) {
    if (type.formattype == FORMAT_VideoInfo && type.cbFormat >= sizeof(VIDEOINFOHEADER)) {
        auto *info = reinterpret_cast<VIDEOINFOHEADER *>(type.pbFormat);
        if (interval) *interval = &info->AvgTimePerFrame;
        return &info->bmiHeader;
    }
    if (type.formattype == FORMAT_VideoInfo2 && type.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        auto *info = reinterpret_cast<VIDEOINFOHEADER2 *>(type.pbFormat);
        if (interval) *interval = &info->AvgTimePerFrame;
        return &info->bmiHeader;
    }
    return nullptr;
}
struct Device { CameraInfo info; ComPtr<IMoniker> moniker; };
QVector<Device> devices() {
    ComPtr<ICreateDevEnum> system;
    checkDs(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&system)), "DirectShow 枚举器不可用");
    ComPtr<IEnumMoniker> enumerator;
    auto hr = system->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumerator, 0);
    if (hr == S_FALSE) return {};
    checkDs(hr, "DirectShow 枚举失败");
    QVector<Device> result;
    for (;;) {
        ComPtr<IMoniker> moniker;
        if (enumerator->Next(1, &moniker, nullptr) != S_OK) break;
        ComPtr<IPropertyBag> bag;
        if (FAILED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) continue;
        auto property = [&](const wchar_t *key) {
            VARIANT value; VariantInit(&value);
            QString text;
            if (SUCCEEDED(bag->Read(key, &value, nullptr)) && value.vt == VT_BSTR) text = QString::fromWCharArray(value.bstrVal);
            VariantClear(&value); return text;
        };
        QString id = property(L"DevicePath"), name = property(L"FriendlyName");
        if (id.isEmpty()) {
            LPOLESTR text = nullptr;
            if (SUCCEEDED(moniker->GetDisplayName(nullptr, nullptr, &text))) { id = QString::fromWCharArray(text); CoTaskMemFree(text); }
        }
        if (!id.isEmpty()) result.append({{"dshow:" + id, name.isEmpty() ? "DirectShow Camera" : name}, moniker});
    }
    return result;
}
class Callback final : public GrabberCallback {
  public:
    Callback() { event = CreateEventW(nullptr, FALSE, FALSE, nullptr); if (!event) fail("创建视频事件失败。"); }
    ~Callback() { CloseHandle(event); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (id == IID_IUnknown || id == __uuidof(GrabberCallback)) { *value = static_cast<GrabberCallback *>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE *data, long size) override {
        if (!data || size < 1 || size > 128 * 1024 * 1024) return E_INVALIDARG;
        try { std::lock_guard<std::mutex> guard(mutex); bytes = QByteArray(reinterpret_cast<char *>(data), size); }
        catch (...) { return E_OUTOFMEMORY; }
        SetEvent(event); return S_OK;
    }
    HANDLE event;
    std::mutex mutex;
    QByteArray bytes;
  private: std::atomic<ULONG> refs_{1};
};
class DirectShowCamera final : public ICameraSource {
  public:
    explicit DirectShowCamera(CameraOptions options) : options_(options) {}
    ~DirectShowCamera() override { close(); }
    void close() {
        if (control_) control_->Stop();
        if (grabber_) grabber_->SetCallback(nullptr, 1);
        event_.Reset(); control_.Reset(); graph_.Reset(); builder_.Reset();
        source_.Reset(); sink_.Reset(); filter_.Reset(); grabber_.Reset(); callback_.Reset();
    }
    void open(const QString &id) override {
        ComPtr<IMoniker> moniker;
        for (const auto &device : devices()) if (device.info.id.mid(6).compare(id, Qt::CaseInsensitive) == 0) moniker = device.moniker;
        if (!moniker) fail("未找到 DirectShow 摄像头；OBS 需安装虚拟摄像头组件。");
        QString lastError;
        for (const auto &subtype : {MEDIASUBTYPE_NV12, MEDIASUBTYPE_YUY2, MEDIASUBTYPE_RGB32, MEDIASUBTYPE_RGB24, MEDIASUBTYPE_MJPG}) {
            try { openGraph(moniker.Get(), subtype); return; }
            catch (const std::exception &e) { lastError = QString::fromUtf8(e.what()); close(); }
        }
        fail(lastError);
    }
    void openGraph(IMoniker *moniker, const GUID &subtype) {
        checkDs(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph_)), "创建 DirectShow 图失败");
        checkDs(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&builder_)), "创建采集图失败");
        checkDs(builder_->SetFiltergraph(graph_.Get()), "设置采集图失败");
        checkDs(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&source_)), "摄像头无法打开或被占用");
        checkDs(graph_->AddFilter(source_.Get(), L"Camera"), "加入摄像头失败");
        ComPtr<IAMStreamConfig> config;
        if (SUCCEEDED(builder_->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, source_.Get(), IID_PPV_ARGS(&config)))) {
            int count = 0, bytes = 0;
            struct Mode { double cost; AM_MEDIA_TYPE *type; };
            QVector<Mode> modes;
            if (SUCCEEDED(config->GetNumberOfCapabilities(&count, &bytes)) && count <= 4096 && bytes >= int(sizeof(VIDEO_STREAM_CONFIG_CAPS)) && bytes < 65536) {
                QByteArray caps(bytes, 0);
                for (int i = 0; i < count; ++i) {
                    AM_MEDIA_TYPE *type = nullptr;
                    if (FAILED(config->GetStreamCaps(i, &type, reinterpret_cast<BYTE *>(caps.data())))) continue;
                    REFERENCE_TIME *interval = nullptr;
                    auto *info = bitmap(*type, &interval);
                    if (info && info->biWidth >= 160 && info->biWidth <= 4096 && std::abs(info->biHeight) >= 120 && std::abs(info->biHeight) <= 4096) {
                        auto *limits = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS *>(caps.data());
                        if (limits->MinFrameInterval > 0 && limits->MaxFrameInterval >= limits->MinFrameInterval)
                            *interval = std::clamp<REFERENCE_TIME>(10000000 / options_.fps, limits->MinFrameInterval, limits->MaxFrameInterval);
                        modes.append({cameraModeScore(info->biWidth, std::abs(info->biHeight), *interval > 0 ? 1e7 / *interval : 0, options_), type});
                    } else { clearType(*type); CoTaskMemFree(type); }
                }
                std::sort(modes.begin(), modes.end(), [](const Mode &a, const Mode &b) { return a.cost < b.cost; });
                for (auto mode : modes) if (SUCCEEDED(config->SetFormat(mode.type))) break;
                for (auto mode : modes) { clearType(*mode.type); CoTaskMemFree(mode.type); }
            }
        }
        checkDs(CoCreateInstance(grabberClass, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&filter_)), "Windows Sample Grabber 不可用");
        checkDs(filter_.As(&grabber_), "读取采集接口失败");
        AM_MEDIA_TYPE desired{}; desired.majortype = MEDIATYPE_Video; desired.subtype = subtype;
        checkDs(grabber_->SetMediaType(&desired), "设置输出格式失败");
        checkDs(graph_->AddFilter(filter_.Get(), L"Frames"), "加入采集器失败");
        checkDs(CoCreateInstance(nullRendererClass, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sink_)), "创建帧输出失败");
        checkDs(graph_->AddFilter(sink_.Get(), L"Sink"), "加入帧输出失败");
        checkDs(builder_->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, source_.Get(), filter_.Get(), sink_.Get()), "设备格式无法连接到采集器");
        AM_MEDIA_TYPE actual{};
        checkDs(grabber_->GetConnectedMediaType(&actual), "读取视频格式失败");
        REFERENCE_TIME *interval = nullptr;
        auto *info = bitmap(actual, &interval);
        if (!info) { clearType(actual); fail("DirectShow 返回了未知视频布局。"); }
        width_ = info->biWidth; height_ = std::abs(info->biHeight);
        fps_ = *interval > 0 ? 1e7 / *interval : 0;
        const bool rgb = subtype == MEDIASUBTYPE_RGB24 || subtype == MEDIASUBTYPE_RGB32;
        format_ = subtype == MEDIASUBTYPE_RGB24 ? PixelFormat::Bgr24 : subtype == MEDIASUBTYPE_RGB32 ? PixelFormat::Bgr32 : subtype == MEDIASUBTYPE_YUY2 ? PixelFormat::Yuyv : subtype == MEDIASUBTYPE_NV12 ? PixelFormat::Nv12 : PixelFormat::Jpeg;
        name_ = rgb ? (format_ == PixelFormat::Bgr24 ? "RGB24" : "RGB32") : subtype == MEDIASUBTYPE_YUY2 ? "YUY2" : subtype == MEDIASUBTYPE_NV12 ? "NV12" : "MJPEG";
        const int channels = format_ == PixelFormat::Bgr24 ? 3 : format_ == PixelFormat::Bgr32 ? 4 : format_ == PixelFormat::Yuyv ? 2 : 1;
        stride_ = rgb ? ((width_ * channels + 3) & ~3) : width_ * channels;
        if (rgb && info->biHeight > 0) stride_ = -stride_;
        bt709_ = fullRange_ = false;
        if (actual.formattype == FORMAT_VideoInfo2 && actual.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
            const auto flags = reinterpret_cast<VIDEOINFOHEADER2 *>(actual.pbFormat)->dwControlFlags;
            if (flags & AMCONTROL_COLORINFO_PRESENT) {
                DXVA_ExtendedFormat color{};
                static_assert(sizeof(color) == sizeof(flags));
                std::memcpy(&color, &flags, sizeof(color));
                bt709_ = color.VideoTransferMatrix == DXVA_VideoTransferMatrix_BT709;
                fullRange_ = color.NominalRange == DXVA_NominalRange_0_255;
            }
        }
        clearType(actual);
        if (width_ < 1 || height_ < 1 || width_ > 8192 || height_ > 8192) fail("设备返回的分辨率无效。");
        callback_.Attach(new Callback);
        checkDs(grabber_->SetBufferSamples(FALSE), "设置帧缓存失败");
        checkDs(grabber_->SetOneShot(FALSE), "设置连续采集失败");
        checkDs(grabber_->SetCallback(callback_.Get(), 1), "设置帧回调失败");
        checkDs(graph_.As(&control_), "读取设备控制接口失败");
        graph_.As(&event_);
        // 相机帧以到达时刻采集，避免虚拟设备的时戳被渲染器当作未来播放时间等待。
        ComPtr<IMediaFilter> mediaFilter;
        if (SUCCEEDED(graph_.As(&mediaFilter))) mediaFilter->SetSyncSource(nullptr);
        checkDs(control_->Run(), "DirectShow 摄像头启动失败");
    }
    QImage read() override {
        if (event_) {
            long code = 0; LONG_PTR p1 = 0, p2 = 0;
            while (event_->GetEvent(&code, &p1, &p2, 0) == S_OK) {
                event_->FreeEventParams(code, p1, p2);
                if (code == EC_ERRORABORT || code == EC_DEVICE_LOST) fail("DirectShow 摄像头断开或读取失败。");
            }
        }
        if (WaitForSingleObject(callback_->event, 100) != WAIT_OBJECT_0) return {};
        QByteArray bytes;
        { std::lock_guard<std::mutex> guard(callback_->mutex); bytes = callback_->bytes; }
        return decodeCameraFrame(reinterpret_cast<const uchar *>(bytes.constData()), bytes.size(), width_, height_, stride_, format_, bt709_, fullRange_);
    }
    QString description() const override { return QString("DirectShow · %1×%2 · %3 fps · %4").arg(width_).arg(height_).arg(fps_, 0, 'f', 1).arg(name_); }
  private:
    ComRuntime runtime_;
    CameraOptions options_;
    ComPtr<IGraphBuilder> graph_;
    ComPtr<ICaptureGraphBuilder2> builder_;
    ComPtr<IBaseFilter> source_, filter_, sink_;
    ComPtr<SampleGrabber> grabber_;
    ComPtr<IMediaControl> control_;
    ComPtr<IMediaEvent> event_;
    ComPtr<Callback> callback_;
    int width_ = 0, height_ = 0, stride_ = 0;
    double fps_ = 0;
    PixelFormat format_ = PixelFormat::Bgr32;
    bool bt709_ = false, fullRange_ = false;
    QString name_;
};
}
QVector<CameraInfo> enumerateDirectShowCameras() {
    ComRuntime runtime; QVector<CameraInfo> result;
    for (const auto &device : devices()) result.append(device.info);
    return result;
}
std::unique_ptr<ICameraSource> createDirectShowCamera(const CameraOptions &options) { return std::make_unique<DirectShowCamera>(options); }
}
