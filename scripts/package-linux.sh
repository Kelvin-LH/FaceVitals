#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="${FV_BUILD_DIR:-$root/build/linux}"
ort="${ONNXRUNTIME_ROOT:-$root/external/onnxruntime-linux-x64-1.23.2}"
dest="${1:-$root/dist/FaceVitals-0.6.0-linux-x64}"
test ! -e "$dest" || { echo 'Destination exists; choose a new directory.' >&2; exit 1; }
mkdir -p "$dest/lib" "$dest/models" "$dest/licenses"
cp "$build/FaceVitals" "$dest/"
strip "$dest/FaceVitals"
cp -a "$ort/lib/"libonnxruntime.so* "$dest/lib/"
cp -a "$root/models/bootstrap" "$dest/models/"
cp "$root/README.md" "$root/DEPENDENCIES.md" "$root/LICENSE" "$root/THIRD_PARTY.md" "$root/更新说明_0.6.md" "$root/摄像头与户外演示指南.md" "$dest/"
cp "$ort/LICENSE" "$ort/ThirdPartyNotices.txt" "$dest/licenses/"
cat > "$dest/run.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$dir/FaceVitals" "$@"
EOF
chmod +x "$dest/run.sh"
(cd "$dest" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
du -sh "$dest"
