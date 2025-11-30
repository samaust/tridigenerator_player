#!/usr/bin/env bash
set -e

ANDROID_API=26
NDK_ROOT=$ANDROID_NDK

FFMPEG_DIR=$(pwd)/FFmpeg
BUILD_DIR=$(pwd)/FFmpeg/build
INSTALL_DIR=$2/FFmpeg/install
DAV1D_DIR=$1

echo $ANDROID_API
echo $NDK_ROOT
echo $FFMPEG_DIR
echo $BUILD_DIR
echo $INSTALL_DIR
echo $DAV1D_DIR

mkdir -p "$BUILD_DIR"
mkdir -p "$INSTALL_DIR"

export PKG_CONFIG=pkg-config
export PKG_CONFIG_LIBDIR="$DAV1D_DIR/install/lib/pkgconfig"
export PKG_CONFIG_PATH="$DAV1D_DIR/install/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=""

echo $PKG_CONFIG_LIBDIR
echo $PKG_CONFIG_PATH

# Patch configure
CONFIGURE_FILE="$FFMPEG_DIR/configure"
PATCH_MARKER="check_func dav1d_version"

# Backup original configure if not already backed up
if [ ! -f "$CONFIGURE_FILE.orig" ]; then
    cp "$CONFIGURE_FILE" "$CONFIGURE_FILE.orig"
fi

# Patch configure only if the marker line is not present
if ! grep -q "$PATCH_MARKER" "$CONFIGURE_FILE"; then
    echo "Patching configure for the first time..."
    sed -i '/enabled libdav1d[[:space:]]*&& require_pkg_config libdav1d "dav1d >= 0.5.0" "dav1d\/dav1d.h"/c\
enabled libdav1d && {\
    add_cflags  "-I'"$DAV1D_DIR"'/install/include"\
    add_ldflags "-L'"$DAV1D_DIR"'/install/lib -Wl,-rpath-link='"$DAV1D_DIR"'/install/lib"\
\
    check_headers dav1d/dav1d.h || die "dav1d header not found"\
\
    # check_func: check_func <function> <header> <libs>\
    check_func dav1d_version "-ldav1d" || die "dav1d library not found"\
}' "$CONFIGURE_FILE"
else
    echo "Configure already patched, skipping..."
fi
crcrcrcrcdfdsfac
# Configure FFmpeg
cd ${FFMPEG_DIR}
./configure \
    --logfile=config.log \
    --prefix="$INSTALL_DIR" \
    --target-os=android \
    --arch=aarch64 \
    --enable-cross-compile \
    --cross-prefix="${NDK_ROOT}/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android-" \
    --cc="${NDK_ROOT}/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android${ANDROID_API}-clang" \
    --nm="${NDK_ROOT}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm" \
    --ar="${NDK_ROOT}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar" \
    --ranlib="${NDK_ROOT}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ranlib" \
    --strip="${NDK_ROOT}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" \
    --pkg-config-flags="--static" \
    --extra-cflags="-I${DAV1D_DIR}/install/include" \
    --extra-ldflags="-L${DAV1D_DIR}/install/lib -Wl,-rpath-link=${DAV1D_DIR}/install/lib" \
    --enable-version3 \
    --disable-gpl \
    --disable-nonfree \
    --enable-libdav1d \
    --enable-demuxer=matroska,webm \
    --enable-decoder=av1 \
    --disable-programs \
    --disable-doc \
    --enable-shared

# Build and install FFmpeg
make -j$(nproc)
make install
