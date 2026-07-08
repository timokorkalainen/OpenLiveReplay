#!/usr/bin/env python3
import plistlib
import sys
from pathlib import Path


def require(text: str, needle: str, message: str) -> None:
    if needle not in text:
        raise AssertionError(message)


def main() -> None:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: test_ios_ndi_static.py <CMakeLists.txt> <ndisink.cpp> "
            "<ndiruntimepaths.h> <ios/Info.plist>"
        )

    cmake = Path(sys.argv[1]).read_text(encoding="utf-8")
    ndisink = Path(sys.argv[2]).read_text(encoding="utf-8")
    runtime_paths = Path(sys.argv[3]).read_text(encoding="utf-8")
    info_plist = plistlib.loads(Path(sys.argv[4]).read_bytes())

    require(cmake, "OLR_NDI_IOS_SDK_DIR", "iOS builds must expose an NDI SDK path cache var")
    require(cmake, "OLR_NDI_IOS_REQUIRED", "iOS NDI must have an opt-in required build gate")
    require(cmake, "FATAL_ERROR", "required iOS NDI builds must fail configure when SDK is missing")
    require(cmake, "lib/iOS/libndi_ios.a", "iOS builds must look for the NDI iOS static archive")
    require(cmake, "OLR_NDI_STATIC_LINK=1", "iOS NDI must compile the static-link sender path")
    require(cmake, "-framework Accelerate", "iOS NDI static archive requires Accelerate")
    require(cmake, "-framework Foundation", "iOS NDI static archive requires Foundation")
    if "-ldns_sd" in cmake:
        raise AssertionError("iOS builds must not link unavailable libdns_sd; Bonjour is in libSystem")

    require(ndisink, "#include \"playback/output/ndistaticlink.h\"",
            "NdiOutputSink must include the static-link symbol declarations")
    require(ndisink, "resolveStaticSymbols", "NdiOutputSink must resolve static NDI symbols")
    require(ndisink, "&NDIlib_send_create", "static path must bind NDIlib_send_create")
    require(ndisink, "&NDIlib_send_send_video_v2", "static path must bind NDI video send")
    require(ndisink, "&NDIlib_send_send_audio_v3", "static path must bind NDI audio send")

    require(runtime_paths, "defined(Q_OS_IOS)", "runtime candidates need an explicit iOS branch")
    ios_branch_start = runtime_paths.index("defined(Q_OS_IOS)")
    mac_branch_start = runtime_paths.index("defined(Q_OS_MACOS)")
    ios_branch = runtime_paths[ios_branch_start:mac_branch_start]
    if "libndi.dylib" in ios_branch or "libndi.so" in ios_branch:
        raise AssertionError("iOS must not advertise desktop NDI runtime candidates")

    if "NSLocalNetworkUsageDescription" not in info_plist:
        raise AssertionError("iOS plist must explain local-network use for NDI")
    services = info_plist.get("NSBonjourServices", [])
    if "_ndi._tcp." not in services:
        raise AssertionError("iOS plist must advertise the NDI Bonjour service")

    print("PASS: iOS NDI static-link build and plist wiring are present")


if __name__ == "__main__":
    main()
