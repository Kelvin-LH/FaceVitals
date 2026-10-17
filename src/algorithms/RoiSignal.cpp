#include "RoiSignal.h"
#include <algorithm>

namespace fv {
RoiSignal extractRoiSignal(const QImage &frame, const QRect &face, double time, int requiredMask) {
    RoiSignal result; result.sample.t = time;
    if (frame.isNull() || face.width() < 80 || face.height() < 80) {
        result.sample.qualityReason = "face_too_small"; return result;
    }
    const auto rgb = frame.convertToFormat(QImage::Format_RGB888);
    const QRect rois[] = {
        QRect(face.x()+int(face.width()*.30),face.y()+int(face.height()*.18),int(face.width()*.40),int(face.height()*.13)),
        QRect(face.x()+int(face.width()*.16),face.y()+int(face.height()*.48),int(face.width()*.20),int(face.height()*.17)),
        QRect(face.x()+int(face.width()*.64),face.y()+int(face.height()*.48),int(face.width()*.20),int(face.height()*.17))};
    int regions = 0, retained = 0, total = 0;
    for (int k=0;k<3;++k) {
        if (requiredMask && !(requiredMask & (1<<k))) continue;
        const auto roi=rois[k].intersected(frame.rect());
        if(roi!=rois[k]) continue;
        double r=0,g=0,b=0; int count=0, good=0;
        for(int y=roi.top();y<=roi.bottom();y+=2) for(int x=roi.left();x<=roi.right();x+=2) {
            const auto *p=rgb.constScanLine(y)+x*3; ++count;
            if(p[0]>=250 || p[1]>=250 || p[2]>=250 || std::max({p[0],p[1],p[2]})<22 || p[1]<12) continue;
            r+=p[0];g+=p[1];b+=p[2];++good;
        }
        total+=count; retained+=good;
        if(good<40 || good < .65*count) continue;
        result.sample.r+=r/good; result.sample.g+=g/good; result.sample.b+=b/good;
        result.regionMask|=1<<k; ++regions;
    }
    result.retainedFraction=total?double(retained)/total:0;
    if(requiredMask && result.regionMask!=requiredMask) { result.sample.qualityReason="selected_roi_unavailable"; return result; }
    if(regions<2) { result.sample.qualityReason="exposure_or_occlusion"; return result; }
    result.sample.r/=regions;result.sample.g/=regions;result.sample.b/=regions;
    const double mean=(result.sample.r+result.sample.g+result.sample.b)/3;
    result.sample.valid=mean>28 && mean<225;
    result.sample.qualityReason=result.sample.valid?"valid":"illumination_range";
    return result;
}
}
