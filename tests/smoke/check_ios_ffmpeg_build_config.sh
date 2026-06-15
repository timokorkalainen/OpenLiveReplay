#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT="$ROOT_DIR/build-scripts/build_ffmpeg_ios_srt.sh"
CMAKE="$ROOT_DIR/CMakeLists.txt"

require_in_file() {
    local file="$1"
    local needle="$2"
    if ! grep -Fq -- "$needle" "$file"; then
        echo "missing '$needle' in $file" >&2
        exit 1
    fi
}

reject_in_file() {
    local file="$1"
    local needle="$2"
    if grep -Fq -- "$needle" "$file"; then
        echo "forbidden '$needle' found in $file" >&2
        exit 1
    fi
}

require_in_file "$SCRIPT" 'FFMPEG_VERSION="8.1.1"'
require_in_file "$SCRIPT" '.ffmpeg-8.1.1-securetransport-no-hevc-no-avfilter.stamp'
require_in_file "$SCRIPT" 'BUILD_CONFIG_ID="ffmpeg-8.1.1-securetransport-no-hevc-no-avfilter"'
require_in_file "$SCRIPT" 'EXPECTED_ARCHIVES=('
require_in_file "$SCRIPT" 'all_expected_archives_exist'
require_in_file "$SCRIPT" 'grep -Fxq "$BUILD_CONFIG_ID" "$BUILD_CONFIG_STAMP"'
require_in_file "$SCRIPT" 'printf '"'"'%s\n'"'"' "$BUILD_CONFIG_ID" > "$BUILD_CONFIG_STAMP"'
require_in_file "$SCRIPT" 'libsrt_has_forbidden_crypto_symbols()'
require_in_file "$SCRIPT" '-DENABLE_ENCRYPTION=OFF'

require_in_file "$SCRIPT" '--enable-static'
require_in_file "$SCRIPT" '--disable-shared'
require_in_file "$SCRIPT" '--disable-gpl'
require_in_file "$SCRIPT" '--disable-nonfree'
require_in_file "$SCRIPT" '--disable-autodetect'
require_in_file "$SCRIPT" '--disable-everything'
require_in_file "$SCRIPT" '--disable-avfilter'
require_in_file "$SCRIPT" '--enable-securetransport'
require_in_file "$SCRIPT" '--enable-libsrt'
require_in_file "$SCRIPT" '--enable-protocol=libsrt'
require_in_file "$SCRIPT" '--enable-protocol=rtmp'
require_in_file "$SCRIPT" '--enable-protocol=rtmps'
require_in_file "$SCRIPT" '--enable-protocol=tcp'
require_in_file "$SCRIPT" '--enable-protocol=tls'
require_in_file "$SCRIPT" '--enable-demuxer=flv'
require_in_file "$SCRIPT" '--enable-demuxer=live_flv'
require_in_file "$SCRIPT" '--enable-demuxer=mpegts'
require_in_file "$SCRIPT" '--enable-parser=h264'
require_in_file "$SCRIPT" '--enable-decoder=h264'
require_in_file "$SCRIPT" '--enable-hwaccel=h264_videotoolbox'
require_in_file "$SCRIPT" '--enable-decoder=aac'
require_in_file "$SCRIPT" '--enable-decoder=mpeg2video'
require_in_file "$SCRIPT" '--enable-encoder=mpeg2video'

reject_in_file "$SCRIPT" 'OPENSSL_VERSION='
reject_in_file "$SCRIPT" 'build_openssl'
reject_in_file "$SCRIPT" '--enable-openssl'
reject_in_file "$SCRIPT" 'OPENSSL_ROOT'
reject_in_file "$SCRIPT" '-lssl'
reject_in_file "$SCRIPT" '-lcrypto'
reject_in_file "$SCRIPT" 'MBEDTLS_VERSION='
reject_in_file "$SCRIPT" 'build_mbedtls'
reject_in_file "$SCRIPT" '--enable-mbedtls'
reject_in_file "$SCRIPT" 'Mbed-TLS'
reject_in_file "$SCRIPT" 'mbedtls/releases'
reject_in_file "$SCRIPT" '-DUSE_ENCLIB=mbedtls'
reject_in_file "$SCRIPT" '-DSTATIC_MBEDTLS=ON'
reject_in_file "$SCRIPT" '-DMBEDTLS_INCLUDE_DIR='
reject_in_file "$SCRIPT" '-DMBEDTLS_LIB='
reject_in_file "$SCRIPT" '-DMBEDX509_LIB='
reject_in_file "$SCRIPT" '-DMBEDCRYPTO_LIB='
reject_in_file "$SCRIPT" 'create_dependency_xcframework "libmbedtls"'
reject_in_file "$SCRIPT" 'create_dependency_xcframework "libmbedx509"'
reject_in_file "$SCRIPT" 'create_dependency_xcframework "libmbedcrypto"'
reject_in_file "$SCRIPT" '--enable-gpl'
reject_in_file "$SCRIPT" '--enable-nonfree'
reject_in_file "$SCRIPT" '--enable-shared'
reject_in_file "$SCRIPT" '--disable-static'
reject_in_file "$SCRIPT" '--enable-decoder=hevc'
reject_in_file "$SCRIPT" '--enable-parser=hevc'
reject_in_file "$SCRIPT" '--enable-hwaccel=hevc_videotoolbox'
reject_in_file "$SCRIPT" '--enable-encoder=hevc_videotoolbox'
reject_in_file "$SCRIPT" 'libavfilter'
reject_in_file "$SCRIPT" 'create_dependency_xcframework "libssl"'
reject_in_file "$SCRIPT" 'create_dependency_xcframework "libcrypto"'

require_in_file "$CMAKE" 'add_library(ffmpeg_avcodec STATIC IMPORTED)'
require_in_file "$CMAKE" '.ffmpeg-8.1.1-securetransport-no-hevc-no-avfilter.stamp'
require_in_file "$CMAKE" 'add_custom_target(BuildFFmpegDependencies'
require_in_file "$CMAKE" 'BYPRODUCTS ${FFMPEG_EXPECTED_ARCHIVES}'
require_in_file "$CMAKE" 'DEPENDS "${FFMPEG_SCRIPT}"'
require_in_file "$CMAKE" 'set(FFMPEG_XCODE_SLICE "$(OLR_FFMPEG_SLICE)")'
require_in_file "$CMAKE" 'IMPORTED_LOCATION "${FFMPEG_IOS_DIR}/libavcodec.xcframework/${FFMPEG_XCODE_SLICE}/libavcodec.a"'
require_in_file "$CMAKE" 'IMPORTED_LOCATION "${FFMPEG_IOS_DIR}/libsrt.xcframework/${FFMPEG_XCODE_SLICE}/libsrt.a"'
require_in_file "$CMAKE" 'XCODE_ATTRIBUTE_OLR_FFMPEG_SLICE "ios-arm64"'
require_in_file "$CMAKE" 'XCODE_ATTRIBUTE_OLR_FFMPEG_SLICE[sdk=iphoneos*]'
require_in_file "$CMAKE" 'XCODE_ATTRIBUTE_OLR_FFMPEG_SLICE[sdk=iphonesimulator*]'
require_in_file "$CMAKE" '"-framework Security"'

reject_in_file "$CMAKE" 'FFMPEG_IOS_SLICE'
reject_in_file "$CMAKE" 'IMPORTED_LOCATION "${FFMPEG_IOS_DIR}/libavcodec.xcframework"'
reject_in_file "$CMAKE" 'ffmpeg_mbedtls'
reject_in_file "$CMAKE" 'ffmpeg_mbedx509'
reject_in_file "$CMAKE" 'ffmpeg_mbedcrypto'
reject_in_file "$CMAKE" 'libmbedtls'
reject_in_file "$CMAKE" 'libmbedx509'
reject_in_file "$CMAKE" 'libmbedcrypto'
reject_in_file "$CMAKE" 'add_library(ffmpeg_avcodec SHARED IMPORTED)'
reject_in_file "$CMAKE" 'libavcodec.dylib'
reject_in_file "$CMAKE" 'libavfilter.xcframework'
