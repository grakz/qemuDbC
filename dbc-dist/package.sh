#!/bin/sh
# Build the redistributable QEMU-with-DbC bundles from the two build trees.
#
#   dbc-dist/package.sh <linux-build-dir> <windows-build-dir> <output-dir>
#
# Produces a self-contained Linux tarball and a Windows zip, each carrying
# the qemu-system-x86_64 binary, its firmware blobs and the shared
# libraries it needs, so the result runs without the QEMU source tree.
set -eu

BUILD_LINUX=${1:-build-linux}
BUILD_WIN=${2:-build-win}
OUT=${3:-dbc-dist/out}
VER=$(cat VERSION)
STAMP=$(git rev-parse --short HEAD)

rm -rf "$OUT"
mkdir -p "$OUT"

# ---------------------------------------------------------------- Linux
NAME="qemu-dbc-${VER}-${STAMP}-linux-x86_64"
STAGE="$OUT/$NAME"
rm -rf /tmp/stage-linux
(cd "$BUILD_LINUX" && DESTDIR=/tmp/stage-linux ninja install >/dev/null)

mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/share"
cp /tmp/stage-linux/usr/local/bin/qemu-system-x86_64 "$STAGE/bin/qemu-system-x86_64.bin"
cp /tmp/stage-linux/usr/local/bin/qemu-img "$STAGE/bin/qemu-img.bin"
cp -r /tmp/stage-linux/usr/local/share/qemu "$STAGE/share/qemu"
rm -rf "$STAGE/share/qemu/../applications" "$STAGE/share/qemu/../icons"
strip "$STAGE/bin/qemu-system-x86_64.bin" "$STAGE/bin/qemu-img.bin"

# bundle everything except the C runtime and the dynamic loader
for bin in "$STAGE/bin/qemu-system-x86_64.bin" "$STAGE/bin/qemu-img.bin"; do
    ldd "$bin" | awk '{print $3}' | grep -E '^/' | while read -r lib; do
        case $(basename "$lib") in
            libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*| \
            ld-linux*|libresolv.so.*)
                continue ;;
        esac
        cp -n "$lib" "$STAGE/lib/" 2>/dev/null || true
    done
done

for tool in qemu-system-x86_64 qemu-img; do
    cat > "$STAGE/bin/$tool" <<EOF
#!/bin/sh
# Launcher: prefer the bundled libraries over whatever the host has.
here=\$(cd -- "\$(dirname -- "\$0")" && pwd)
LD_LIBRARY_PATH="\$here/../lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}" \\
    exec "\$here/$tool.bin" "\$@"
EOF
    chmod +x "$STAGE/bin/$tool"
done

# -------------------------------------------------------------- Windows
WNAME="qemu-dbc-${VER}-${STAMP}-win64"
WSTAGE="$OUT/$WNAME"
rm -rf /tmp/stage-win
(cd "$BUILD_WIN" && DESTDIR=/tmp/stage-win ninja install >/dev/null)

# on Windows QEMU lives at the prefix root and looks for its blobs in ./share
mkdir -p "$WSTAGE"
cp /tmp/stage-win/usr/local/qemu-system-x86_64.exe "$WSTAGE/"
cp /tmp/stage-win/usr/local/qemu-img.exe "$WSTAGE/"
cp -r /tmp/stage-win/usr/local/share "$WSTAGE/share"
rm -rf "$WSTAGE/share/applications" "$WSTAGE/share/icons"
x86_64-w64-mingw32-strip "$WSTAGE"/*.exe

for dll in libffi-8.dll libgio-2.0-0.dll libglib-2.0-0.dll libgmodule-2.0-0.dll \
           libgobject-2.0-0.dll libiconv-2.dll libintl-8.dll libpcre2-8-0.dll \
           libpixman-1-0.dll zlib1.dll; do
    cp "/opt/mingw-sysroot/mingw64/bin/$dll" "$WSTAGE/"
done
cp /usr/lib/gcc/x86_64-w64-mingw32/13-posix/libssp-0.dll "$WSTAGE/"
cp /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll "$WSTAGE/"

# ------------------------------------------------------------- archives
cp dbc-dist/README.md "$STAGE/README.md"
cp dbc-dist/README.md "$WSTAGE/README.md"

(cd "$OUT" && XZ_OPT="-6 -T0" tar -cJf "$NAME.tar.xz" "$NAME" && rm -rf "$NAME")
(cd "$OUT" && zip -qr9 "$WNAME.zip" "$WNAME" && rm -rf "$WNAME")
(cd "$OUT" && sha256sum ./*.tar.xz ./*.zip > SHA256SUMS)

ls -la "$OUT"
