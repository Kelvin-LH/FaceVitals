#pragma once
#include "EmotionModel.h"
#include "IlluminationGate.h"
#include "RoiSignal.h"
#include <cmath>

namespace fv {
// 实时采集与离线回放共用同一检测/颜色状态机。
class FaceSignalTracker {
  public:
    void reset() { *this = FaceSignalTracker(); }
    void detected(const QVector<FaceDetection> &faces, double t) {
        faceCount = int(faces.size());
        detection = faceCount == 1 ? faces.first() : FaceDetection{};
        moving_ = largeMotion_ = false;
        if(faceCount != 1) return;
        const auto face = detection.unclippedBox.isEmpty()?detection.box:detection.unclippedBox.toAlignedRect();
        const double shift = previous_.isEmpty() ? 0 :
            (face.center()-previous_.center()).manhattanLength()/double(std::max(1,face.width()));
        const double scale = previous_.isEmpty() ? 0 :
            std::abs(face.width()-previous_.width())/double(std::max(1,face.width()));
        moving_ = shift>.10 || scale>.12;
        largeMotion_ = shift>.25 || scale>.25;
        if(largeMotion_ || (lastSeen_>=0 && t-lastSeen_>.35)) { signalFace_={}; mask_=0; gate_.reset(); largeMotion_=true; }
        lastSeen_=t; previous_=face;
        targetFace_=face;
        if(signalFace_.isEmpty()) signalFace_=face;
    }
    RoiSignal sample(const QImage &frame, double t) {
        RoiSignal out; out.sample.t=t;
        if(faceCount!=1) {
            out.sample.qualityReason=faceCount>1?"multiple_faces":"no_face";
            // 单次检测漏检最多桥接 0.25 秒；多人或持续离脸必须清空窗口。
            out.sample.breakContinuity=faceCount>1 || lastSeen_<0 || t-lastSeen_>.25;
            if(out.sample.breakContinuity) { previous_={};signalFace_={};mask_=0;gate_.reset(); }
            return out;
        }
        if(detection.box.width()<80 || detection.visibleFraction<.65) {
            out.sample.qualityReason="face_too_small_or_clipped"; out.sample.breakContinuity=true;
            mask_=0;gate_.reset(); return out;
        }
        // 在每个视频帧上平滑浮点坐标，避免 5 Hz 检测框阶跃污染脉搏频谱。
        const double alpha=lastSample_<0?1:1-std::exp(-std::max(0.,t-lastSample_)/.45);
        lastSample_=t;
        signalFace_=QRectF(signalFace_.x()+alpha*(targetFace_.x()-signalFace_.x()),
            signalFace_.y()+alpha*(targetFace_.y()-signalFace_.y()),
            signalFace_.width()+alpha*(targetFace_.width()-signalFace_.width()),
            signalFace_.height()+alpha*(targetFace_.height()-signalFace_.height()));
        out=extractRoiSignal(frame,signalFace_.toAlignedRect(),t,mask_);
        if(!mask_ && out.sample.valid) {
            // 优先固定双颊，避免额头头发/反光使区域集合来回切换。
            mask_=(out.regionMask&6)==6?6:out.regionMask;
            out=extractRoiSignal(frame,signalFace_.toAlignedRect(),t,mask_);
        }
        if(out.sample.valid && !gate_.accept(out.sample)) {
            out.sample.valid=false;out.sample.breakContinuity=gate_.majorChange();out.sample.qualityReason="lighting_change";
        }
        if(moving_) { out.sample.valid=false;out.sample.qualityReason="motion"; }
        if(largeMotion_) out.sample.breakContinuity=true;
        return out;
    }
    FaceDetection detection;
    int faceCount=0;
  private:
    QRect previous_;
    QRectF signalFace_,targetFace_;
    double lastSeen_=-1,lastSample_=-1;
    int mask_=0;
    bool moving_=false,largeMotion_=false;
    IlluminationGate gate_;
};
}
