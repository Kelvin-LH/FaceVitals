"""Development-only RGB frame decoder for fv_replay_check.

stdout is binary: FVRP0001 + uint32 JSON length + UTF-8 metadata; each packet is
struct <4sQdIII (tag, source frame, source timestamp seconds, width, height,
payload bytes), followed by tightly packed RGB888. FRM1 is a frame, END1 ends
the stream. No facial or physiological model is run in this Python helper.
"""
import argparse
import json
import math
import struct
import sys
import cv2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--video", required=True)
    parser.add_argument("--seconds", type=float, default=120)
    parser.add_argument("--fps", type=float, default=30)
    args = parser.parse_args()
    if not (0 < args.seconds <= 3600 and 0 < args.fps <= 60):
        parser.error("seconds must be in (0,3600], fps in (0,60]")
    capture = cv2.VideoCapture(args.video)
    if not capture.isOpened():
        raise RuntimeError("Cannot open the requested source video")
    source_fps = capture.get(cv2.CAP_PROP_FPS)
    if not math.isfinite(source_fps) or source_fps <= 0:
        raise RuntimeError("Source video has no usable frame rate")
    width, height = (int(capture.get(p)) for p in
                     (cv2.CAP_PROP_FRAME_WIDTH, cv2.CAP_PROP_FRAME_HEIGHT))
    if not (0 < width <= 8192 and 0 < height <= 8192):
        raise RuntimeError("Unsupported video dimensions")
    metadata = {"protocol": "FVRP0001", "opencv_version": cv2.__version__,
                "source_fps": source_fps, "source_frames": int(capture.get(cv2.CAP_PROP_FRAME_COUNT)),
                "width": width, "height": height, "format": "RGB888",
                "timestamp_source": "CAP_PROP_POS_MSEC after decode",
                "requested_output_fps": args.fps, "seconds_exclusive": args.seconds}
    encoded = json.dumps(metadata).encode("utf-8")
    out = sys.stdout.buffer
    out.write(b"FVRP0001" + struct.pack("<I", len(encoded)) + encoded)
    out.flush()
    source_index, emitted, previous_time, next_time = 0, 0, -1.0, 0.0
    packet = struct.Struct("<4sQdIII")
    while True:
        ok, frame = capture.read()
        if not ok:
            break
        timestamp = capture.get(cv2.CAP_PROP_POS_MSEC) / 1000.0
        if not math.isfinite(timestamp) or (source_index and timestamp <= previous_time):
            raise RuntimeError("Non-increasing or invalid source timestamp; no timestamp fabrication allowed")
        previous_time = timestamp
        if timestamp >= args.seconds - 1e-8:
            break
        if timestamp + 1e-8 >= next_time:
            rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            if rgb.shape != (height, width, 3):
                raise RuntimeError("Video dimensions changed during decode")
            payload = rgb.tobytes(order="C")
            out.write(packet.pack(b"FRM1", source_index, timestamp, width, height, len(payload)))
            out.write(payload)
            out.flush()
            emitted += 1
            next_time = emitted / args.fps
        source_index += 1
    out.write(packet.pack(b"END1", source_index, previous_time, width, height, 0))
    out.flush()
    capture.release()
    print(json.dumps({"decoded_until_source_index": source_index, "emitted_frames": emitted}), file=sys.stderr)


if __name__ == "__main__":
    try:
        main()
    except BrokenPipeError:
        sys.exit(1)
    except Exception as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
