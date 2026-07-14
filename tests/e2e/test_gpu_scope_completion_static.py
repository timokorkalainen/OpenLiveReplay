#!/usr/bin/env python3
import re
import sys
from pathlib import Path


TOKEN_RE = re.compile(r"[A-Za-z_]\w*|::|->|==|!=|&&|\|\||[{}()\[\].,;:&*!<>+=/-]")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def strip_comments_and_literals(source):
    """Remove comments and quoted contents while preserving offsets and newlines."""
    chars = list(source)
    index = 0
    state = "code"
    quote = ""
    while index < len(chars):
        current = chars[index]
        following = chars[index + 1] if index + 1 < len(chars) else ""
        if state == "code":
            if current == "/" and following == "/":
                chars[index] = chars[index + 1] = " "
                index += 2
                state = "line_comment"
                continue
            if current == "/" and following == "*":
                chars[index] = chars[index + 1] = " "
                index += 2
                state = "block_comment"
                continue
            if current in ('"', "'"):
                quote = current
                chars[index] = " "
                index += 1
                state = "literal"
                continue
        elif state == "line_comment":
            if current == "\n":
                state = "code"
            else:
                chars[index] = " "
        elif state == "block_comment":
            if current == "*" and following == "/":
                chars[index] = chars[index + 1] = " "
                index += 2
                state = "code"
                continue
            if current != "\n":
                chars[index] = " "
        else:
            if current == "\\":
                chars[index] = " "
                if index + 1 < len(chars) and chars[index + 1] != "\n":
                    chars[index + 1] = " "
                index += 2
                continue
            chars[index] = " " if current != "\n" else "\n"
            if current == quote:
                state = "code"
        index += 1
    require(state not in ("block_comment", "literal"), "unterminated comment or literal")
    return "".join(chars)


def matching_character(source, opening, open_char, close_char):
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == open_char:
            depth += 1
        elif source[index] == close_char:
            depth -= 1
            if depth == 0:
                return index
    raise AssertionError(f"unmatched {open_char!r} at offset {opening}")


def function_block(source, signature):
    cleaned = strip_comments_and_literals(source)
    start = cleaned.find(signature)
    require(start >= 0, f"could not locate function {signature!r}")
    opening = cleaned.find("{", start)
    require(opening >= 0, f"could not locate body for {signature!r}")
    closing = matching_character(cleaned, opening, "{", "}")
    return cleaned[opening:closing + 1]


def tokens(source):
    return [(match.group(0), match.start()) for match in TOKEN_RE.finditer(source)]


def token_pairs(items, open_token, close_token):
    stack = []
    pairs = {}
    for index, (token, _) in enumerate(items):
        if token == open_token:
            stack.append(index)
        elif token == close_token:
            require(stack, f"unmatched token {close_token!r}")
            opening = stack.pop()
            pairs[opening] = index
            pairs[index] = opening
    require(not stack, f"unmatched token {open_token!r}")
    return pairs


def is_obviously_unreachable(items, call_index, brace_pairs):
    depth = 0
    statement_start = 0
    returned_at_depth = set()
    for index, (token, _) in enumerate(items[:call_index]):
        if token == "{":
            depth += 1
            statement_start = index + 1
        elif token == "}":
            returned_at_depth.discard(depth)
            depth -= 1
            statement_start = index + 1
        elif token == ";":
            statement = [value for value, _ in items[statement_start:index + 1]]
            if statement and statement[0] == "return":
                returned_at_depth.add(depth)
            statement_start = index + 1
    if depth in returned_at_depth:
        return True

    for opening, closing in brace_pairs.items():
        if opening >= closing or not (opening < call_index < closing):
            continue
        prefix = [value for value, _ in items[max(0, opening - 4):opening]]
        if prefix == ["if", "(", "false", ")"]:
            return True
    return False


def structured_read_regions(function, label):
    items = tokens(function)
    parens = token_pairs(items, "(", ")")
    braces = token_pairs(items, "{", "}")
    regions = []
    for index, (token, _) in enumerate(items):
        if token != "withRead" or index + 1 >= len(items) or items[index + 1][0] != "(":
            continue
        call_close = parens[index + 1]
        lambda_open = next(
            (candidate for candidate in range(index + 2, call_close)
             if items[candidate][0] == "{"),
            None,
        )
        require(lambda_open is not None, f"{label}: withRead has no callback body")
        lambda_close = braces[lambda_open]
        require(lambda_close < call_close, f"{label}: callback escapes withRead call")
        require(not is_obviously_unreachable(items, index, braces),
                f"{label}: withRead is unreachable")
        regions.append((items[lambda_open][1], items[lambda_close][1]))
    require(regions, f"{label}: no reachable withRead callback")
    return function, regions


def in_any_region(position, regions):
    return any(start < position < stop for start, stop in regions)


def audit_structured_access(function, label, required_identifiers=()):
    function, regions = structured_read_regions(function, label)
    items = tokens(function)
    for token, position in items:
        if token == "nativeHandle":
            require(in_any_region(position, regions),
                    f"{label}: nativeHandle access escapes withRead callback")
        if token in ("read", "complete"):
            require(False, f"{label}: path-sensitive manual {token} is forbidden")
    for identifier in required_identifiers:
        positions = [position for token, position in items if token == identifier]
        require(positions and all(in_any_region(position, regions) for position in positions),
                f"{label}: {identifier} must execute inside withRead callback")
    return function, regions


def expect_rejected(source, message):
    try:
        audit_structured_access(function_block(source, "bool probe"), "mutation", ("use",))
    except AssertionError:
        return
    raise AssertionError(message)


def mutation_self_tests():
    safe = """
bool probe() {
    GpuSyncReadScope scope;
    return scope.withRead(surface, [&](const GpuReadLease& lease) {
        use(lease.nativeHandle());
        return true;
    });
}
"""
    audit_structured_access(function_block(safe, "bool probe"), "safe mutation", ("use",))
    expect_rejected("""
bool probe() {
    GpuSyncReadScope scope;
    auto lease = scope.read(surface);
    if (failure) return false;
    use(lease.nativeHandle());
    scope.complete();
    return true;
}
""", "manual read with an early return must be rejected")
    expect_rejected("""
bool probe() {
    GpuSyncReadScope scope;
    auto lease = scope.read(surface);
    use(lease.nativeHandle());
    // scope.withRead(surface, [&](auto& lease) { use(lease.nativeHandle()); });
    const char* fake = "scope.withRead nativeHandle use";
    return false;
}
""", "comment/string markers must not satisfy the audit")
    expect_rejected("""
bool probe() {
    return false;
    GpuSyncReadScope scope;
    return scope.withRead(surface, [&](const GpuReadLease& lease) {
        use(lease.nativeHandle());
        return true;
    });
}
""", "unreachable withRead markers must not satisfy the audit")


def audit_exact_synchronization_flow(function, label, require_derived_pair=False,
                                     require_pair_fields=True):
    items = tokens(function)
    braces = token_pairs(items, "{", "}")
    sync_indices = [index for index, (token, _) in enumerate(items)
                    if token == "gpuSynchronization"]
    require(sync_indices, f"{label}: missing IFrameData exact synchronization evidence")
    require(any(not is_obviously_unreachable(items, index, braces) for index in sync_indices),
            f"{label}: exact synchronization evidence is unreachable")
    item_values = [token for token, _ in items]
    require("pendingFenceValue" not in item_values,
            f"{label}: surface-wide pending watermark is forbidden")
    require("gpuFence" not in item_values,
            f"{label}: independently selected fence is forbidden")
    require("isExact" in item_values,
            f"{label}: exact evidence must be validated before use")
    if require_pair_fields:
        require("fence" in item_values and "value" in item_values,
                f"{label}: exact evidence must consume one fence/value pair")
    if require_derived_pair:
        require(re.search(r"fenceValue\s*=\s*synchronization\s*\.\s*value", function),
                f"{label}: fence value must come from synchronization")
        require(re.search(r"producerFence\s*=\s*synchronization\s*\.\s*fence", function),
                f"{label}: producer fence must come from the same synchronization")


def exact_synchronization_mutation_self_tests():
    safe = """
bool probe() {
    const GpuFrameSynchronization synchronization = data->gpuSynchronization();
    const uint64_t fenceValue = synchronization.value;
    auto producerFence = synchronization.fence;
    return synchronization.isExact() && producerFence->wait(fenceValue, 1);
}
"""
    audit_exact_synchronization_flow(function_block(safe, "bool probe"), "safe exact mutation",
                                     True)
    for source, message in (("""
bool probe() {
    const GpuFrameSynchronization synchronization = data->gpuSynchronization();
    const uint64_t fenceValue = surface->pendingFenceValue();
    auto producerFence = synchronization.fence;
    return synchronization.isExact() && producerFence->wait(fenceValue, 1);
}
""", "surface watermark fallback must be rejected"), ("""
bool probe() {
    const GpuFrameSynchronization synchronization = data->gpuSynchronization();
    const uint64_t fenceValue = synchronization.value;
    auto producerFence = unrelatedFence;
    return synchronization.isExact() && producerFence->wait(fenceValue, 1);
}
""", "independent fence/value pairing must be rejected"), ("""
bool probe() {
    return false;
    const GpuFrameSynchronization synchronization = data->gpuSynchronization();
    const uint64_t fenceValue = synchronization.value;
    auto producerFence = synchronization.fence;
    return synchronization.isExact() && producerFence->wait(fenceValue, 1);
}
""", "unreachable exact evidence must be rejected")):
        try:
            audit_exact_synchronization_flow(function_block(source, "bool probe"), "mutation",
                                             True)
        except AssertionError:
            continue
        raise AssertionError(message)


def audit_apple_fence_factory(source, label):
    factory = function_block(source, "std::shared_ptr<GpuFence> GpuFence::create()")
    capture = re.search(
        r"const\s+uint64_t\s+(\w+)\s*=\s*[^;]*"
        r"currentDeviceAuthorityEpoch\s*\(\s*\)\s*;",
        factory,
    )
    require(capture is not None, f"{label}: authority must be captured explicitly")
    authority = capture.group(1)
    require(len(re.findall(r"\bcurrentDeviceAuthorityEpoch\s*\(", factory)) == 1,
            f"{label}: authority must be read exactly once before native creation")

    device = re.search(r"\bMTLCreateSystemDefaultDevice\s*\(", factory)
    queue = re.search(r"id\s*<\s*MTLCommandQueue\s*>\s+(\w+)\s*=\s*"
                      r"\[[^\]]+\s+newCommandQueue\s*\]", factory)
    require(device is not None and queue is not None,
            f"{label}: default Metal device and queue creation must remain explicit")
    require(capture.start() < device.start() < queue.start(),
            f"{label}: authority capture must precede native device and queue creation")
    require(re.search(rf"if\s*\(\s*{re.escape(authority)}\s*==\s*0\s*\)\s*"
                      r"return\s+nullptr\s*;", factory[capture.end():device.start()]),
            f"{label}: zero authority must fail before native creation")

    queue_name = queue.group(1)
    validation = re.search(
        rf"if\s*\(\s*!\s*[^)]*isCurrentDeviceAuthority\s*\(\s*"
        rf"{re.escape(authority)}\s*\)\s*\)\s*\{{(?P<body>.*?)\}}",
        factory,
        re.DOTALL,
    )
    require(validation is not None and validation.start() > queue.end(),
            f"{label}: created queue must be rejected if captured authority changed")
    require(re.search(rf"\[\s*{re.escape(queue_name)}\s+release\s*\]",
                      validation.group("body")) and
            re.search(r"return\s+nullptr\s*;", validation.group("body")),
            f"{label}: stale queue rejection must release ownership and fail closed")

    factory_call = re.search(rf"makeMetalGpuFence\s*\(\s*{re.escape(queue_name)}\s*,\s*"
                             rf"{re.escape(authority)}\s*\)", factory)
    require(factory_call is not None and validation.end() < factory_call.start(),
            f"{label}: validated captured authority must be passed explicitly to the fence")
    release_positions = [match.start() for match in re.finditer(
        rf"\[\s*{re.escape(queue_name)}\s+release\s*\]", factory)]
    require(any(position > factory_call.end() for position in release_positions),
            f"{label}: successful queue ownership must be released after fence creation")


def apple_fence_factory_mutation_self_tests():
    safe = """
std::shared_ptr<GpuFence> GpuFence::create() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    const uint64_t authorityEpoch = monitor.currentDeviceAuthorityEpoch();
    if (authorityEpoch == 0) return nullptr;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [device newCommandQueue];
    [device release];
    if (!queue) return nullptr;
    if (!monitor.isCurrentDeviceAuthority(authorityEpoch)) {
        [queue release];
        return nullptr;
    }
    auto fence = makeMetalGpuFence(queue, authorityEpoch);
    [queue release];
    return fence;
}
"""
    audit_apple_fence_factory(safe, "safe Apple fence factory mutation")
    late_capture = safe.replace(
        "    const uint64_t authorityEpoch = monitor.currentDeviceAuthorityEpoch();\n"
        "    if (authorityEpoch == 0) return nullptr;\n",
        "",
    ).replace(
        "    id<MTLCommandQueue> queue = [device newCommandQueue];\n",
        "    id<MTLCommandQueue> queue = [device newCommandQueue];\n"
        "    const uint64_t authorityEpoch = monitor.currentDeviceAuthorityEpoch();\n"
        "    if (authorityEpoch == 0) return nullptr;\n",
    )
    missing_validation = re.sub(
        r"    if \(!monitor\.isCurrentDeviceAuthority\(authorityEpoch\)\) \{\n"
        r"        \[queue release\];\n        return nullptr;\n    \}\n",
        "",
        safe,
    )
    late_read = safe.replace(
        "makeMetalGpuFence(queue, authorityEpoch)",
        "makeMetalGpuFence(queue, monitor.currentDeviceAuthorityEpoch())",
    )
    for mutation, message in (
        (late_capture, "late authority capture must be rejected"),
        (missing_validation, "missing post-creation validation must be rejected"),
        (late_read, "late authority read at fence construction must be rejected"),
    ):
        try:
            audit_apple_fence_factory(mutation, "Apple fence factory mutation")
        except AssertionError:
            continue
        raise AssertionError(message)


def main():
    if len(sys.argv) != 15:
        raise SystemExit(
            "usage: test_gpu_scope_completion_static.py "
            "<nativevideoencoder_videotoolbox.mm> "
            "<nativevideoencoder_mediafoundation.cpp> "
            "<applegpusurface_apple.mm> <wingpuimportedge.cpp> "
            "<gpuframedata.cpp> <gpusurfaceallocator.cpp> "
            "<vtkeepsurfaceimporter_apple.mm> <gpucompositor.cpp> "
            "<asyncgpureadbacksink.cpp> <decklinksink.cpp> <outputbusengine.cpp> "
            "<gpuframeretirequeue.cpp> <gpuencodepump.cpp> <gpufence_apple.mm>"
        )

    mutation_self_tests()
    exact_synchronization_mutation_self_tests()
    apple_fence_factory_mutation_self_tests()
    videotoolbox = Path(sys.argv[1]).read_text(encoding="utf-8")
    mediafoundation = Path(sys.argv[2]).read_text(encoding="utf-8")
    apple_surface = Path(sys.argv[3]).read_text(encoding="utf-8")
    win_import = Path(sys.argv[4]).read_text(encoding="utf-8")
    frame_data = Path(sys.argv[5]).read_text(encoding="utf-8")
    allocator = Path(sys.argv[6]).read_text(encoding="utf-8")
    vt_importer = Path(sys.argv[7]).read_text(encoding="utf-8")
    compositor = Path(sys.argv[8]).read_text(encoding="utf-8")
    async_readback = Path(sys.argv[9]).read_text(encoding="utf-8")
    decklink = Path(sys.argv[10]).read_text(encoding="utf-8")
    output_bus = Path(sys.argv[11]).read_text(encoding="utf-8")
    retire_queue = Path(sys.argv[12]).read_text(encoding="utf-8")
    encode_pump = Path(sys.argv[13]).read_text(encoding="utf-8")
    apple_fence = Path(sys.argv[14]).read_text(encoding="utf-8")

    audit_apple_fence_factory(apple_fence, "Apple default Metal fence factory")

    vt_encode, vt_regions = audit_structured_access(
        function_block(videotoolbox, "bool encodeSurface"),
        "VideoToolbox encodeSurface",
        ("CVPixelBufferCreateWithIOSurface", "qScopeGuard", "encodePixelBuffer"),
    )
    vt_tokens = tokens(vt_encode)
    vt_parens = token_pairs(vt_tokens, "(", ")")
    vt_braces = token_pairs(vt_tokens, "{", "}")
    scope_guard_index = next(index for index, (token, _) in enumerate(vt_tokens)
                             if token == "qScopeGuard")
    require(vt_tokens[scope_guard_index + 1][0] == "(",
            "VideoToolbox qScopeGuard must own a release callback")
    guard_call_close = vt_parens[scope_guard_index + 1]
    guard_lambda_open = next(index for index in range(scope_guard_index + 2, guard_call_close)
                             if vt_tokens[index][0] == "{")
    guard_lambda_close = vt_braces[guard_lambda_open]
    release_positions = [position for token, position in vt_tokens
                         if token == "CVPixelBufferRelease"]
    require(len(release_positions) == 1 and
            vt_tokens[guard_lambda_open][1] < release_positions[0] <
            vt_tokens[guard_lambda_close][1],
            "VideoToolbox CVPixelBuffer release must be owned only by qScopeGuard")
    positions = {identifier: next(position for token, position in vt_tokens
                                  if token == identifier)
                 for identifier in ("CVPixelBufferCreateWithIOSurface", "qScopeGuard",
                                    "encodePixelBuffer")}
    require(positions["CVPixelBufferCreateWithIOSurface"] < positions["qScopeGuard"] <
            positions["encodePixelBuffer"] and
            all(in_any_region(position, vt_regions) for position in positions.values()),
            "VideoToolbox pixel-buffer RAII must arm before the throwing callback path")

    audit_structured_access(
        function_block(mediafoundation, "bool MediaFoundationEncoder::buildSurfaceSample"),
        "Media Foundation buildSurfaceSample", ("MFCreateDXGISurfaceBuffer",))
    audit_structured_access(
        function_block(apple_surface, "CVPixelBufferRef retainApplePixelBufferWrapper"),
        "Apple wrapper creation", ("CVPixelBufferCreateWithIOSurface",))
    audit_structured_access(
        function_block(win_import, "WinGpuImportEdge::createFenceForSurface"),
        "Windows fence creation", ("makeD3D11GpuFence",))
    win_readback, win_regions = audit_structured_access(
        function_block(win_import, "CpuPlanes D3D11IGpuFrameData::readToCpu"),
        "Windows readback", ("Map", "Unmap"))
    readback_tokens = tokens(win_readback)
    map_position = next(position for token, position in readback_tokens if token == "Map")
    unmap_position = next(position for token, position in readback_tokens if token == "Unmap")
    require(map_position < unmap_position and in_any_region(unmap_position, win_regions),
            "Windows readback must unmap before its structured callback completes")

    exact_wait = function_block(frame_data, "bool GpuFrameData::waitForPendingFence")
    exact_wait_tokens = [token for token, _ in tokens(exact_wait)]
    require("pendingFenceValue" not in exact_wait_tokens,
            "generic GPU frame waits must never consult the shared surface watermark")
    require(exact_wait_tokens.count("m_renderFenceValue") >= 2 and
            "m_renderFence" in exact_wait_tokens and "wait" in exact_wait_tokens,
            "generic GPU frame waits must use their stored exact fence/value pair")

    allocator_mint = function_block(allocator, "std::shared_ptr<GpuRhiContext> rhi")
    require(re.search(r"makeGpuFrameHandle\s*\([^;]*result\s*\.\s*producerFence[^;]*"
                      r"result\s*\.\s*fenceValue", allocator_mint,
                      re.DOTALL),
            "allocator mint must carry the exact submission fence/value pair into GpuFrameData")
    vt_import = function_block(vt_importer, "FrameHandle importVtSurface")
    require(re.search(r"exactRenderFence\s*=\s*result\s*\.\s*producerFence", vt_import) and
            re.search(r"renderFenceValue\s*=\s*result\s*\.\s*fenceValue", vt_import) and
            re.search(r"makeGpuFrameHandle\s*\([^;]*exactRenderFence[^;]*renderFenceValue",
                      vt_import, re.DOTALL),
            "VT import must carry the exact submission fence/value pair into GpuFrameData")
    composite = function_block(compositor, "FrameHandle GpuCompositor::composeGridForGeneration")
    require(re.search(r"makeGpuFrameHandle\s*\([^;]*submission\s*\.\s*producerFence[^;]*"
                      r"submission\s*\.\s*fenceValue", composite,
                      re.DOTALL),
            "compositor output must carry the exact submission fence/value pair into GpuFrameData")

    audit_exact_synchronization_flow(
        function_block(async_readback, "bool AsyncGpuReadbackSink::submit("),
        "asynchronous readback submit", True)
    audit_exact_synchronization_flow(
        function_block(async_readback, "bool AsyncGpuReadbackSink::submitGpuFrameAndFlush"),
        "asynchronous readback flush", True)
    audit_exact_synchronization_flow(
        function_block(async_readback, "bool AsyncGpuReadbackSink::prewarmReadback"),
        "asynchronous readback prewarm", True)
    audit_exact_synchronization_flow(
        function_block(decklink, "bool waitForNativeGpuProducer"), "DeckLink producer wait")
    audit_exact_synchronization_flow(
        function_block(output_bus, "OutputBusFrame OutputBusEngine::renderSingleSource"),
        "output bus reuse", require_pair_fields=False)
    audit_exact_synchronization_flow(
        function_block(retire_queue, "void GpuFrameRetireQueue::collect"),
        "frame retirement queue")

    pump_submit = function_block(encode_pump, "bool GpuEncodePump::submit")
    audit_exact_synchronization_flow(
        pump_submit, "GPU encode pump submit", require_pair_fields=False)
    require(re.search(r"Job\s+job\s*\{[^;]*synchronization", pump_submit, re.DOTALL),
            "GPU encode pump must store the frame's exact synchronization as one job field")
    pump_run = function_block(encode_pump, "void GpuEncodePump::run")
    pump_run_tokens = [token for token, _ in tokens(pump_run)]
    require("gpuFence" not in pump_run_tokens and "fenceValue" not in pump_run_tokens and
            "m_fence" not in pump_run_tokens,
            "GPU encode pump must not reconstruct or fall back from the exact job pair")
    require(re.search(r"job\s*\.\s*synchronization\s*\.\s*fence\s*->\s*wait\s*\(\s*"
                      r"job\s*\.\s*synchronization\s*\.\s*value", pump_run),
            "GPU encode pump must wait on the fence and value from the same job synchronization")

    print("PASS: parsed GPU scope and exact-fence contracts are mutation-resistant")


if __name__ == "__main__":
    main()
