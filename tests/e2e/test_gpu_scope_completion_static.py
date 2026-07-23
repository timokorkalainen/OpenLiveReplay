#!/usr/bin/env python3
import os
import re
import sys
import tempfile
from pathlib import Path


TOKEN_RE = re.compile(r"[A-Za-z_]\w*|\d+|::|->|==|!=|&&|\|\||[{}()\[\].,;:&*!<>+=/-]")

FIRST_PARTY_SOURCE_SUFFIXES = frozenset({
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp",
    ".m", ".mm",
})
FIRST_PARTY_EXCLUDED_PARTS = frozenset({
    "deps", "dependencies", "docs", "external", "handoff-notes", "node_modules",
    "tests", "third_party", "third-party", "vendor", "vendors", "linux_build",
    "windows_build", "_deps",
})


def is_first_party_source_path(relative):
    path = Path(relative)
    if path.suffix.lower() not in FIRST_PARTY_SOURCE_SUFFIXES:
        return False
    for part in path.parts[:-1]:
        lowered = part.lower()
        if (lowered in FIRST_PARTY_EXCLUDED_PARTS or lowered == "build"
                or lowered.startswith("build-")
                or lowered.startswith("cmake-build") or lowered.startswith(".")):
            return False
    return True


def first_party_source_paths(repo_root):
    for current, directories, files in os.walk(repo_root):
        current_path = Path(current)
        relative_current = current_path.relative_to(repo_root)
        directories[:] = [
            directory for directory in directories
            if is_first_party_source_path(relative_current / directory / "probe.cpp")
        ]
        for filename in files:
            path = current_path / filename
            if is_first_party_source_path(path.relative_to(repo_root)):
                yield path


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
            raw = (re.match(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(', source[index:])
                   if current in ("L", "R", "U", "u") else None)
            if raw is not None:
                terminator = ")" + raw.group(1) + '"'
                closing = source.find(terminator, index + raw.end())
                literal_end = len(source) if closing < 0 else closing + len(terminator)
                for literal_index in range(index, literal_end):
                    if chars[literal_index] != "\n":
                        chars[literal_index] = " "
                index = literal_end
                continue
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


def direct_enclosing_brace(brace_pairs, item_index):
    enclosing = [opening for opening, closing in brace_pairs.items()
                 if opening < closing and opening < item_index < closing]
    return max(enclosing) if enclosing else None


def lambda_body_openings(items, brace_pairs):
    bracket_pairs = token_pairs(items, "[", "]")
    lambda_bodies = set()
    for body_open, body_close in brace_pairs.items():
        if body_open >= body_close:
            continue
        boundary = max(
            (index for index in range(body_open)
             if items[index][0] in (";", "{", "}")),
            default=-1,
        )
        for capture_open in range(boundary + 1, body_open):
            if items[capture_open][0] != "[":
                continue
            capture_close = bracket_pairs[capture_open]
            if capture_close >= body_open:
                continue
            previous = items[capture_open - 1][0] if capture_open > boundary + 1 else None
            if previous in ("=", "(", ",", "return", "{", ":") or previous is None:
                lambda_bodies.add(body_open)
                break
    return lambda_bodies


def is_inside_nested_lambda(items, brace_pairs, item_index, permitted_body=None):
    for body_open in lambda_body_openings(items, brace_pairs):
        if body_open == permitted_body:
            continue
        if body_open < item_index < brace_pairs[body_open]:
            return True
    return False


def is_inside_conditional_block(items, brace_pairs, item_index):
    paren_pairs = token_pairs(items, "(", ")")
    for body_open, body_close in brace_pairs.items():
        if body_open >= body_close or not body_open < item_index < body_close:
            continue
        previous = body_open - 1
        if previous >= 0 and items[previous][0] == "else":
            return True
        if previous < 0 or items[previous][0] != ")":
            continue
        condition_open = paren_pairs[previous]
        keyword = condition_open - 1
        if keyword >= 0 and items[keyword][0] == "constexpr":
            keyword -= 1
        if keyword >= 0 and items[keyword][0] in ("if", "while", "switch"):
            return True
    return False


def is_obviously_unreachable(items, call_index, brace_pairs):
    def has_dead_guard_prefix(values):
        dead_guards = (
            ("if", "(", "false", ")"),
            ("if", "(", "0", ")"),
            ("while", "(", "false", ")"),
            ("while", "(", "0", ")"),
            ("if", "constexpr", "(", "false", ")"),
            ("if", "constexpr", "(", "0", ")"),
        )
        return any(len(values) >= len(guard) and tuple(values[-len(guard):]) == guard
                   for guard in dead_guards)

    depth = 0
    statement_start = 0
    terminated_at_depth = set()
    for index, (token, _) in enumerate(items[:call_index]):
        if token == "{":
            depth += 1
            statement_start = index + 1
        elif token == "}":
            terminated_at_depth.discard(depth)
            depth -= 1
            statement_start = index + 1
        elif token == ";":
            statement = [value for value, _ in items[statement_start:index + 1]]
            if statement and statement[0] in ("return", "throw"):
                terminated_at_depth.add(depth)
            statement_start = index + 1
    # A return in any still-open ancestor block also makes code in a newly
    # nested child block unreachable. Returns from closed sibling blocks are
    # removed when their closing brace is visited above.
    if any(terminal_depth <= depth for terminal_depth in terminated_at_depth):
        return True

    if has_dead_guard_prefix([value for value, _ in items[max(0, call_index - 6):call_index]]):
        return True

    for opening, closing in brace_pairs.items():
        if opening >= closing or not (opening < call_index < closing):
            continue
        prefix = [value for value, _ in items[max(0, opening - 6):opening]]
        if has_dead_guard_prefix(prefix):
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
    braces = token_pairs(items, "{", "}")

    def direct_in_read_region(item_index):
        position = items[item_index][1]
        for start, stop in regions:
            if not start < position < stop:
                continue
            callback_body = next(
                (index for index, (token, token_position) in enumerate(items)
                 if token == "{" and token_position == start),
                None,
            )
            if (callback_body is not None and
                    direct_enclosing_brace(braces, item_index) == callback_body):
                return True
        return False

    for index, (token, _) in enumerate(items):
        if token == "nativeHandle":
            require(direct_in_read_region(index),
                    f"{label}: nativeHandle access escapes withRead callback")
        if token in ("read", "complete"):
            require(False, f"{label}: path-sensitive manual {token} is forbidden")
    for identifier in required_identifiers:
        indices = [index for index, (token, _) in enumerate(items) if token == identifier]
        require(indices and all(direct_in_read_region(index) for index in indices),
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
    bool success = false;
    GpuSyncReadScope scope;
    scope.withRead(surface, [&](const GpuReadLease& lease) {
        void* handle = lease.nativeHandle();
        use(handle);
        success = true;
    });
    return success;
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
    scope.withRead(surface, [&](const GpuReadLease& lease) {
        void* handle = lease.nativeHandle();
        use(handle);
    });
    return true;
}
""", "unreachable withRead markers must not satisfy the audit")
    expect_rejected("""
bool probe() {
    GpuSyncReadScope scope;
    scope.withRead(surface, [&](const GpuReadLease& lease) {
        auto neverCalled = [&] {
            void* handle = lease.nativeHandle();
            use(handle);
        };
    });
    return true;
}
""", "native access hidden in an uninvoked nested lambda must be rejected")


def audit_exact_synchronization_flow(function, label, require_derived_pair=False,
                                     require_pair_fields=True):
    items = tokens(function)
    braces = token_pairs(items, "{", "}")
    def is_live_evidence(index):
        return (not is_obviously_unreachable(items, index, braces) and
                not is_inside_nested_lambda(items, braces, index))

    def live_indices(value):
        return [index for index, (token, _) in enumerate(items)
                if token == value and is_live_evidence(index)]

    def live_match(pattern):
        for match in re.finditer(pattern, function):
            item_index = next(
                (index for index, (_, position) in enumerate(items)
                 if position >= match.start()),
                None,
            )
            if item_index is not None and is_live_evidence(item_index):
                return True
        return False

    synchronization_declarations = list(re.finditer(
        r"(?:const\s+)?GpuFrameSynchronization\s+(\w+)\s*=\s*"
        r"[^;]*gpuSynchronization\s*\(\s*\)[^;]*;",
        function,
        re.DOTALL,
    ))
    require(synchronization_declarations,
            f"{label}: missing IFrameData exact synchronization evidence")

    def expression_is_live(pattern):
        return live_match(pattern)

    bound_synchronization = None
    for declaration in synchronization_declarations:
        candidate = declaration.group(1)
        escaped = re.escape(candidate)
        if not expression_is_live(rf"\b{escaped}\s*\.\s*isExact\s*\("):
            continue
        if require_pair_fields and not (
                expression_is_live(rf"\b{escaped}\s*\.\s*fence\b") and
                expression_is_live(rf"\b{escaped}\s*\.\s*value\b")):
            continue
        bound_synchronization = candidate
        break
    require(bound_synchronization is not None,
            f"{label}: exact fence, value, and validation must come from one synchronization")
    item_values = [token for token, _ in items]
    require("pendingFenceValue" not in item_values,
            f"{label}: surface-wide pending watermark is forbidden")
    require("gpuFence" not in item_values,
            f"{label}: independently selected fence is forbidden")
    if require_derived_pair:
        escaped = re.escape(bound_synchronization)
        require(live_match(rf"fenceValue\s*=\s*{escaped}\s*\.\s*value"),
                f"{label}: fence value must come from synchronization")
        require(live_match(rf"producerFence\s*=\s*{escaped}\s*\.\s*fence"),
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
""", "unreachable exact evidence must be rejected"), ("""
bool probe() {
    auto neverCalled = [&] {
        const GpuFrameSynchronization synchronization = data->gpuSynchronization();
        const uint64_t fenceValue = synchronization.value;
        auto producerFence = synchronization.fence;
        return synchronization.isExact() && producerFence->wait(fenceValue, 1);
    };
    return false;
}
""", "exact synchronization evidence in an uninvoked lambda must be rejected"), ("""
bool probe() {
    const GpuFrameSynchronization synchronization = data->gpuSynchronization();
    const GpuFrameSynchronization unrelated = other->gpuSynchronization();
    if (!synchronization.isExact()) return false;
    return unrelated.fence->wait(synchronization.value, 1);
}
""", "split exact fence/value pair must be rejected")):
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

    devices = list(re.finditer(
        r"id\s*<\s*MTLDevice\s*>\s+(\w+)\s*=\s*MTLCreateSystemDefaultDevice\s*\(",
        factory,
    ))
    require(len(devices) == 1, f"{label}: factory must create exactly one default Metal device")
    device = devices[0]
    device_name = device.group(1)
    queue = re.search(r"id\s*<\s*MTLCommandQueue\s*>\s+(\w+)\s*=\s*"
                      r"\[\s*(\w+)\s+newCommandQueue\s*\]", factory)
    require(device is not None and queue is not None,
            f"{label}: default Metal device and queue creation must remain explicit")
    require(queue.group(2) == device_name,
            f"{label}: command queue must be created from the captured default device")
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
    factory_items = tokens(factory)
    factory_braces = token_pairs(factory_items, "{", "}")
    factory_body = next(
        (index for index, (token, _) in enumerate(factory_items) if token == "{"), None)
    device_token = next(
        (index for index, (token, position) in enumerate(factory_items)
         if token == "MTLCreateSystemDefaultDevice" and position >= device.start()),
        None,
    )
    queue_token = next(
        (index for index, (token, position) in enumerate(factory_items)
         if token == "newCommandQueue" and position >= queue.start()),
        None,
    )
    require(device_token is not None and queue_token is not None and
            direct_enclosing_brace(factory_braces, device_token) == factory_body and
            direct_enclosing_brace(factory_braces, queue_token) == factory_body and
            not is_obviously_unreachable(factory_items, device_token, factory_braces) and
            not is_obviously_unreachable(factory_items, queue_token, factory_braces),
            f"{label}: Metal device and queue creation must execute directly in the factory")
    validation_token = next(
        (index for index, (token, position) in enumerate(factory_items)
         if token == "isCurrentDeviceAuthority" and position >= validation.start()),
        None,
    )
    require(validation_token is not None and
            direct_enclosing_brace(factory_braces, validation_token) == factory_body and
            not is_obviously_unreachable(factory_items, validation_token, factory_braces),
            f"{label}: stale queue validation must execute directly in the factory")
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
    dead_validation = safe.replace(
        "    if (!monitor.isCurrentDeviceAuthority(authorityEpoch)) {\n"
        "        [queue release];\n"
        "        return nullptr;\n"
        "    }",
        "    if (false) {\n"
        "        if (!monitor.isCurrentDeviceAuthority(authorityEpoch)) {\n"
        "            [queue release];\n"
        "            return nullptr;\n"
        "        }\n"
        "    }",
    )
    dead_device_creation = safe.replace(
        "    id<MTLDevice> device = MTLCreateSystemDefaultDevice();",
        "    id<MTLDevice> device = nil;\n"
        "    if (false) { device = MTLCreateSystemDefaultDevice(); }",
    )
    wrong_queue_device = safe.replace("[device newCommandQueue]",
                                      "[otherDevice newCommandQueue]")
    for mutation, message in (
        (late_capture, "late authority capture must be rejected"),
        (missing_validation, "missing post-creation validation must be rejected"),
        (late_read, "late authority read at fence construction must be rejected"),
        (dead_validation, "unreachable stale-queue validation must be rejected"),
        (dead_device_creation, "unreachable Metal device creation must be rejected"),
        (wrong_queue_device, "queue creation from the wrong Metal device must be rejected"),
    ):
        try:
            audit_apple_fence_factory(mutation, "Apple fence factory mutation")
        except AssertionError:
            continue
        raise AssertionError(message)


def audit_bound_apple_surface_domain(source, label):
    constructor = function_block(source, "AppleGpuSurface(")
    surface_class = function_block(source, "class AppleGpuSurface")
    compatibility = function_block(
        source, "GpuSurfaceCompatibility compatibility() const override")
    compatibility_tokens = [token for token, _ in tokens(compatibility)]
    require("m_deviceDomainId" in compatibility_tokens,
            f"{label}: compatibility must return the cached device domain")
    require("gpuMetalDeviceDomainId" not in compatibility_tokens and
            "m_device" not in compatibility_tokens and "registryID" not in compatibility_tokens,
            f"{label}: compatibility must not query the Objective-C device")
    require("MTLCreateSystemDefaultDevice" not in strip_comments_and_literals(source) and
            "defaultMetalDeviceDomainId" not in strip_comments_and_literals(source),
            f"{label}: surfaces must never infer compatibility from the global default device")
    surface_tokens = [token for token, _ in tokens(surface_class)]
    require("m_device" not in surface_tokens,
            f"{label}: surfaces must not create or retain a Metal device")
    constructor_prefix = strip_comments_and_literals(source)
    constructor_start = constructor_prefix.find("AppleGpuSurface(")
    constructor_open = constructor_prefix.find("{", constructor_start)
    constructor_declaration = constructor_prefix[constructor_start:constructor_open]
    require(re.search(r"GpuSurfaceCompatibility\s+(\w+)", constructor_declaration) is not None,
            f"{label}: construction must require explicit compatibility")
    binding_name = re.search(
        r"GpuSurfaceCompatibility\s+(\w+)", constructor_declaration).group(1)
    require(re.search(rf"m_deviceDomainId\s*\(\s*{re.escape(binding_name)}\s*\.\s*"
                      r"deviceDomainId\s*\)", constructor_declaration) and
            re.search(rf"m_authorityEpoch\s*\(\s*{re.escape(binding_name)}\s*\.\s*"
                      r"authorityEpoch\s*\)", constructor_declaration),
            f"{label}: surface evidence must copy the passed domain and authority exactly")

    for factory_signature in ("std::shared_ptr<GpuSurface> makeAppleNv12Surface",
                              "std::shared_ptr<GpuSurface> makeAppleRgba8Surface",
                              "std::shared_ptr<GpuSurface> wrapAppleImageBuffer"):
        factory = function_block(source, factory_signature)
        declaration_start = strip_comments_and_literals(source).find(factory_signature)
        declaration_open = strip_comments_and_literals(source).find("{", declaration_start)
        declaration = strip_comments_and_literals(source)[declaration_start:declaration_open]
        passed = re.search(r"GpuSurfaceCompatibility\s+(\w+)", declaration)
        require(passed is not None and
                re.search(rf"make_shared\s*<\s*AppleGpuSurface\s*>\s*\([^;]*"
                          rf"{re.escape(passed.group(1))}\s*\)", factory, re.DOTALL),
                f"{label}: {factory_signature} must forward explicit compatibility")


def bound_apple_surface_domain_mutation_self_tests():
    safe = """
class AppleGpuSurface {
public:
    AppleGpuSurface(CVPixelBufferRef pixelBuffer, FramePixelFormat format,
                    GpuSurfaceCompatibility compatibility)
        : m_deviceDomainId(compatibility.deviceDomainId),
          m_authorityEpoch(compatibility.authorityEpoch) {}
    GpuSurfaceCompatibility compatibility() const override {
        return {m_deviceDomainId, m_authorityEpoch};
    }
private:
    uintptr_t m_deviceDomainId = 0;
    uint64_t m_authorityEpoch = 0;
};
std::shared_ptr<GpuSurface> makeAppleNv12Surface(
        int width, int height, GpuSurfaceCompatibility compatibility) {
    return std::make_shared<AppleGpuSurface>(pb, FramePixelFormat::Nv12, compatibility);
}
std::shared_ptr<GpuSurface> makeAppleRgba8Surface(
        int width, int height, GpuSurfaceCompatibility compatibility) {
    return std::make_shared<AppleGpuSurface>(pb, FramePixelFormat::Rgba8, compatibility);
}
std::shared_ptr<GpuSurface> wrapAppleImageBuffer(
        void* image, GpuSurfaceCompatibility compatibility) {
    return std::make_shared<AppleGpuSurface>(pb, FramePixelFormat::Nv12, compatibility);
}
"""
    audit_bound_apple_surface_domain(safe, "safe bound Apple domain mutation")
    for mutation, message in (
        (safe.replace("m_deviceDomainId, m_authorityEpoch",
                      "gpuMetalDeviceDomainId(device), m_authorityEpoch"),
         "per-query Objective-C domain lookup must be rejected"),
        (safe.replace("class AppleGpuSurface {",
                      "uintptr_t defaultMetalDeviceDomainId() {\n"
                      "    static const uintptr_t id = MTLCreateSystemDefaultDevice().registryID;\n"
                      "    return id;\n}\nclass AppleGpuSurface {"),
         "global default-device caching must be rejected"),
        (safe.replace("m_authorityEpoch(compatibility.authorityEpoch)",
                      "m_authorityEpoch(currentDeviceAuthorityEpoch())"),
         "surface authority must come from the bound context"),
        (safe.replace("m_deviceDomainId(compatibility.deviceDomainId)",
                      "m_deviceDomainId(0)"),
         "zeroing the passed device domain must be rejected"),
        (safe.replace("FramePixelFormat::Rgba8, compatibility",
                      "FramePixelFormat::Rgba8, {}"),
         "dropping factory compatibility must be rejected"),
        (safe.replace("m_deviceDomainId, m_authorityEpoch",
                      "m_deviceDomainId + device.registryID, m_authorityEpoch"),
         "compatibility Objective-C device access must be rejected"),
    ):
        try:
            audit_bound_apple_surface_domain(mutation, "bound Apple domain mutation")
        except AssertionError:
            continue
        raise AssertionError(message)


def audit_apple_rhi_surface_compatibility(source, label):
    method = function_block(source, "GpuSurfaceCompatibility GpuRhiContext::surfaceCompatibility")
    require("MTLCreateSystemDefaultDevice" not in method and "static" not in
            [token for token, _ in tokens(method)],
            f"{label}: compatibility must be read from each context, never a global cache")
    require(re.search(r"!\s*isGpuBacked\s*\(\s*\)", method) and
            re.search(r"m_impl\s*->\s*backend\s*!=\s*QRhi\s*::\s*Metal", method) and
            re.search(r"!\s*m_impl\s*->\s*metalCommandQueueBound", method) and
            re.search(r"!\s*m_readbackFence", method),
            f"{label}: only a valid Metal QRhi with a proven native queue may bind surfaces")
    identity = re.search(r"(?:const\s+)?GpuFenceIdentity\s+(\w+)\s*=\s*"
                         r"m_readbackFence\s*->\s*identity\s*\(\s*\)\s*;", method)
    require(identity is not None and
            re.search(rf"return\s*\{{\s*{re.escape(identity.group(1))}\s*\.\s*"
                      rf"deviceDomainId\s*,\s*{re.escape(identity.group(1))}\s*\.\s*"
                      r"authorityEpoch\s*\}\s*;", method),
            f"{label}: binding must copy the actual context fence domain and authority")

    fence_factory = function_block(source, "std::shared_ptr<GpuFence> GpuRhiContext::createFence")
    require(re.search(r"!\s*nativeHandles\s*\|\|\s*!\s*nativeHandles\s*->\s*cmdQueue",
                      fence_factory) and
            re.search(r"m_impl\s*->\s*metalCommandQueueBound\s*=\s*true", fence_factory) and
            re.search(r"makeMetalGpuFence\s*\(\s*nativeHandles\s*->\s*cmdQueue",
                      fence_factory),
            f"{label}: queue provenance must be recorded only from QRhi Metal native handles")


def audit_apple_rhi_readback_cleanup(source, label):
    frame_guard = function_block(source, "class AppleReadbackFrameGuard")
    end_method = function_block(frame_guard, "QRhi::FrameOpResult end()")
    disarm = end_method.find("m_frameOpen = false")
    end_call = end_method.find("endOffscreenFrame")
    require(disarm >= 0 and end_call > disarm,
            f"{label}: Apple frame cleanup must disarm before ambiguous endOffscreenFrame")
    require("m_complete" not in frame_guard and "complete()" not in frame_guard,
            f"{label}: Apple frame cleanup must not carry an inert completion latch")

    batch_guard = function_block(source, "class AppleReadbackBatchGuard")
    require(re.search(r"catch\s*\(\s*\.\.\.\s*\)\s*\{[^}]*"
                      r"(?:static_cast\s*<\s*void\s*>|\(\s*void\s*\))",
                      batch_guard, re.DOTALL),
            f"{label}: Apple batch cleanup catch must document an intentional no-op")

    for signature in ("CpuPlanes readbackRgba8WithRhi", "CpuPlanes readbackNv12WithRhi"):
        block = function_block(source, signature)
        begin = block.find("beginOffscreenFrame")
        frame_guard = block.find("AppleReadbackFrameGuard")
        batch_guard = block.find("AppleReadbackBatchGuard")
        transfer = block.find("batchGuard.take()")
        finish = block.find("rhi->finish()")
        require(begin >= 0 and frame_guard > begin,
                f"{label}: {signature} must arm cleanup after beginOffscreenFrame")
        require(batch_guard > frame_guard and transfer > batch_guard,
                f"{label}: {signature} must guard the update batch until QRhi accepts it")
        require(finish > transfer,
                f"{label}: {signature} must synchronously finish accepted readback work")


def audit_apple_timed_device_loss_poll(source, label):
    render_thread_name = ("class GpuRenderThread" if "class GpuRenderThread" in source
                          else "class D3DRenderThread")
    render_thread = function_block(source, render_thread_name)
    require("invokeFor" in render_thread and "wait_for" in render_thread,
            f"{label}: render thread must expose a bounded invocation path")
    synchronous_invoke = function_block(render_thread, "bool invoke(")
    require("invokeFor" not in synchronous_invoke and
            "make_shared" not in synchronous_invoke and
            "wait_for" not in synchronous_invoke and
            re.search(r"InvokeState\s+\w+\s*;", synchronous_invoke) and
            re.search(r"m_jobs\s*\.\s*append", synchronous_invoke) and
            re.search(r"finished\s*\.\s*wait", synchronous_invoke),
            f"{label}: ordinary synchronous invokes must use caller-owned state, "
            "not the allocating timed path")
    require(re.search(r"try\s*\{\s*std\s*::\s*unique_lock[^;]*;[^}]*"
                      r"finished\s*\.\s*wait.*\}\s*catch\s*"
                      r"\(\s*\.\.\.\s*\)\s*\{[^}]*std\s*::\s*terminate",
                      synchronous_invoke, re.DOTALL),
            f"{label}: caller-stack wait lock construction and wait must fail-stop")
    poll = function_block(source, "bool GpuRhiContext::pollDeviceLoss() const")
    require("invokeFor" in poll and re.search(r"\[\s*impl\s*,\s*authority\s*\]", poll),
            f"{label}: pollDeviceLoss must use timed value-captured render work")
    require("[&]" not in poll,
            f"{label}: timed-out poll work must not retain reference captures")
    require(re.search(r"try\s*\{[^}]*invokeFor.*\}\s*catch\s*\(\s*\.\.\.\s*\)\s*\{"
                      r"[^}]*deviceLossPollPending[^}]*false",
                      poll, re.DOTALL),
            f"{label}: pre-enqueue failure must re-arm device-loss polling")

    if "class GpuRenderThread" in source:
        require("struct QueuedJob" in render_thread and
                re.search(r"void\s+quarantine\s*\(\s*\)", render_thread) and
                re.search(r"cancelled\s*\.\s*swap\s*\(\s*m_jobs\s*\)", render_thread) and
                re.search(r"job\s*\.\s*cancel\s*\(\s*\)", render_thread) and
                re.search(r"\[\s*state\s*,\s*completion\s*,\s*finish\s*\]\s*\{"
                          r"[^}]*finish\s*\(\s*completion\s*\)", render_thread, re.DOTALL),
                f"{label}: timed queue jobs must be cancellable during quarantine")
        require("m_abandonCleanup" in render_thread and "delete rhi" in render_thread,
                f"{label}: quarantined render threads must abandon QRhi cleanup")
        destructor = function_block(source, "GpuRhiContext::~GpuRhiContext()")
        require(re.search(r"unique_ptr\s*<\s*Impl\s*>\s+\w+\s*=\s*std\s*::\s*move\s*"
                          r"\(\s*m_impl\s*\)", destructor) and
                re.search(r"thread\s*\.\s*wait\s*\(\s*100\s*\)", destructor) and
                re.search(r"thread\s*\.\s*quarantine\s*\(\s*\)", destructor) and
                ".release()" in destructor,
                f"{label}: context teardown must bound join then quarantine a live carrier")
        hot_path = function_block(source, "bool GpuRhiContext::invokeOnRenderThread")
        require("invokeFor" not in hot_path and
                re.search(r"thread\s*\.\s*invoke\s*\(\s*\[\s*&\s*\]", hot_path) and
                "renderJob" not in hot_path,
                f"{label}: ordinary synchronous render invokes must remain allocation-free")
        require(re.search(r"invokeFor\s*\(.*?\[\s*impl\s*\]\s*\{[^}]*"
                          r"deviceLossPollPending[^}]*false", poll, re.DOTALL),
                f"{label}: queued poll cancellation must clear the coalescing bit")


def audit_stub_synchronous_invoke_fail_stop(source, label):
    render_thread = function_block(source, "class NullRenderThread")
    synchronous_invoke = function_block(render_thread, "bool invoke(")
    require(re.search(r"try\s*\{\s*std\s*::\s*unique_lock[^;]*;.*"
                      r"m_jobs\s*\.\s*append.*m_cond\s*\.\s*wait.*"
                      r"\}\s*catch\s*\(\s*\.\.\.\s*\)\s*\{[^}]*"
                      r"std\s*::\s*terminate",
                      synchronous_invoke, re.DOTALL),
            f"{label}: accepted stack-captured work must fail-stop across lock/wait setup")


def audit_apple_finite_fence_wait(source, label):
    wait = function_block(source, "bool wait(uint64_t value, int timeoutMs)")
    zero_timeout = wait.find("timeoutMs == 0")
    finite_timeout = wait.find("timeoutMs > 0")
    wait_state = wait.find("struct WaitState")
    listener = wait.find("notifyListener")
    require(0 <= zero_timeout < finite_timeout < wait_state < listener,
            f"{label}: zero and finite waits must return before listener state is installed")
    finite_branch = wait[finite_timeout:wait_state]
    require("signaledValue" in finite_branch and
            "steady_clock" in finite_branch and
            ("sleep_for" in finite_branch or "msleep" in finite_branch),
            f"{label}: finite waits must use bounded allocation-free signaled-value polling")


def audit_compositor_frame_disarm(source, label):
    guard = function_block(source, "class OffscreenFrameGuard")
    finish = function_block(guard, "QRhi::FrameOpResult finish()")
    disarm = finish.find("m_open = false")
    end_call = finish.find("endOffscreenFrame")
    require(disarm >= 0 and end_call > disarm,
            f"{label}: compositor frame guard must disarm before ambiguous endOffscreenFrame")

def audit_terminal_wait_contract(source, label):
    retired = function_block(source, "bool terminalFrameRetired")
    shutdown = function_block(source, "void PlaybackWorker::shutdownGpuOwnersAfterFailure")
    require("completedValue" not in retired and
            re.search(r"fence\s*->\s*wait\s*\([^,]+,\s*0\s*\)", retired),
            f"{label}: terminal probes must use the fence timeout contract")
    require("completedValue" not in shutdown and "fence->wait" in shutdown,
            f"{label}: terminal deadline waits must not call completedValue directly")


def audit_win_import_device_loss_sticky(source, label):
    observation = function_block(source, "bool noteDeviceLostReason")
    publication = re.search(
        r"(?:const\s+)?uint64_t\s+(?P<generation>\w+)\s*=\s*"
        r"WinGpuImportEdge::publishDeviceRemovedForMonitor\s*\(",
        observation,
    )
    sticky_call_pattern = (
        r"(?:deviceLossState\s*->\s*)?deviceLost\s*\.\s*store\s*\(\s*true\s*,"
    )
    sticky_calls = list(re.finditer(sticky_call_pattern, observation))
    gated_sticky = None
    if publication:
        generation = re.escape(publication.group("generation"))
        gated_sticky = re.search(
            rf"if\s*\(\s*{generation}\s*!=\s*0\s*\)\s*(?:\{{\s*)?"
            rf"{sticky_call_pattern}",
            observation[publication.end():],
        )
    gated_end = (publication.end() + gated_sticky.end()
                 if publication and gated_sticky else -1)
    authoritative_return = observation.find("return true", gated_end)
    require(
        publication is not None and gated_sticky is not None and len(sticky_calls) == 1
        and sticky_calls[0].start() >= publication.end()
        and authoritative_return > gated_end,
        f"{label}: sticky loss state must wait for durable monitor proof publication",
    )


def audit_terminal_quarantine_participant_promotion(source, label):
    handoff = function_block(source, "void quarantineTerminalGpuOwners")
    promotion = handoff.find("promoteRecoveryParticipantToTerminal")
    first_store = handoff.find("available->output")
    require(
        promotion >= 0 and first_store >= 0 and promotion < first_store,
        f"{label}: recovery participant must be promoted before terminal slot ownership transfer",
    )


def audit_retire_queue_exception_safety(source, worker, label):
    drain = function_block(source, "int GpuFrameRetireQueue::drain")
    require("QVector<Entry> pending" not in drain and
            re.search(r"\*\s*survivor\s*=\s*std::move\s*\(\s*\*\s*current\s*\)", drain) and
            re.search(r"catch\s*\(\s*\.\.\.\s*\)", drain),
            f"{label}: drain must compact survivors by noexcept move and contain fence exceptions")

    append = function_block(source, "void GpuFrameRetireQueue::append")
    reserve = append.find("m_entries.reserve")
    other_move = append.find("m_entries.append(std::move(entry))")
    clear = append.find("other.m_entries.clear()")
    require(0 <= reserve < other_move < clear and "QVector<Entry> merged" not in append,
            f"{label}: append must reserve before moving entries without shared-owner copies")

    restore = function_block(worker, "void restoreRetireQueueOrTerminate")
    require("owner.append(std::move(local))" in restore and
            "if (!owner.isEmpty()) std::terminate()" not in restore and
            worker.count("restoreRetireQueueOrTerminate") >= 4,
            f"{label}: swap-local drains must merge survivors with concurrent owner arrivals")


def audit_terminal_import_reaping(worker, win_import, label):
    reap = function_block(worker, "void reapTerminalGpuQuarantine")
    poll = function_block(worker, "void pollTerminalSlotBackend")
    clear = function_block(worker, "bool clearTerminalSlot")
    require("pollDeviceLossFor" in poll and "deviceLost()" not in poll,
            f"{label}: terminal import polling must use the deadline-aware edge contract")
    reap_items = tokens(reap)
    reap_braces = token_pairs(reap_items, "{", "}")
    lock_call = next((index for index, (value, _) in enumerate(reap_items)
                      if value == "terminalGpuQuarantineMutex"), None)
    poll_call = next((index for index, (value, _) in enumerate(reap_items)
                      if value == "pollTerminalSlotBackend"), None)
    lock_scope = (direct_enclosing_brace(reap_braces, lock_call)
                  if lock_call is not None else None)
    require(lock_scope is not None and poll_call is not None
            and reap_braces[lock_scope] < poll_call,
            f"{label}: backend polling must execute after the quarantine lock scope")
    require("slot.importRoot.reset()" not in clear and "std::move(slot.importRoot)" in clear,
            f"{label}: import roots must be detached for destruction outside the quarantine lock")
    destructor = function_block(win_import, "WinGpuImportEdge::~WinGpuImportEdge")
    require("CoUninitialize" not in destructor and "coOwned" not in win_import and
            "ScopedComApartment" in win_import,
            f"{label}: COM initialization must be balanced on each calling thread")


def terminal_import_audit_mutation_self_tests():
    safe_sticky = """
bool noteDeviceLostReason(HRESULT reason, uintptr_t domainId) const {
    if (FAILED(reason)) {
        const uint64_t generation = WinGpuImportEdge::publishDeviceRemovedForMonitor(
            reason, deviceAuthorityEpoch, domainId);
        if (generation != 0)
            deviceLossState->deviceLost.store(true, std::memory_order_release);
        return true;
    }
    return false;
}
"""
    audit_win_import_device_loss_sticky(safe_sticky, "shared sticky carrier mutation")
    for unsafe, message in (
        (safe_sticky.replace(
            "        if (generation != 0)\n"
            "            deviceLossState->deviceLost.store(true, std::memory_order_release);",
            "        deviceLossState->deviceLost.store(true, std::memory_order_release);",
        ), "unconditional shared sticky publication must be rejected"),
        (safe_sticky.replace(
            "        const uint64_t generation = WinGpuImportEdge::publishDeviceRemovedForMonitor(\n"
            "            reason, deviceAuthorityEpoch, domainId);\n"
            "        if (generation != 0)\n"
            "            deviceLossState->deviceLost.store(true, std::memory_order_release);",
            "        deviceLossState->deviceLost.store(true, std::memory_order_release);\n"
            "        const uint64_t generation = WinGpuImportEdge::publishDeviceRemovedForMonitor(\n"
            "            reason, deviceAuthorityEpoch, domainId);",
        ), "sticky publication before durable proof must be rejected"),
    ):
        try:
            audit_win_import_device_loss_sticky(unsafe, "unsafe sticky mutation")
        except AssertionError:
            continue
        raise AssertionError(message)

    safe_worker = """
bool clearTerminalSlot() {
    releasedRoots.importRoot = std::move(slot.importRoot);
    return true;
}
void pollTerminalSlotBackend() {
    poll.importRoot->pollDeviceLossFor(10);
}
void reapTerminalGpuQuarantine() {
    TerminalGpuBackendPoll poll;
    {
        std::lock_guard<std::mutex> lock(terminalGpuQuarantineMutex());
        poll = selectOneTerminalBackend();
    }
    pollTerminalSlotBackend(poll);
}
"""
    safe_import = """
class ScopedComApartment {};
WinGpuImportEdge::~WinGpuImportEdge() {
    m_impl.reset();
}
"""
    audit_terminal_import_reaping(safe_worker, safe_import, "unlocked poll mutation")
    unsafe_worker = safe_worker.replace(
        "        poll = selectOneTerminalBackend();\n"
        "    }\n"
        "    pollTerminalSlotBackend(poll);",
        "        poll = selectOneTerminalBackend();\n"
        "        pollTerminalSlotBackend(poll);\n"
        "    }",
    )
    try:
        audit_terminal_import_reaping(unsafe_worker, safe_import, "locked poll mutation")
    except AssertionError:
        pass
    else:
        raise AssertionError("terminal backend polling inside the quarantine lock must be rejected")


def apple_rhi_surface_compatibility_mutation_self_tests():
    safe = """
GpuSurfaceCompatibility GpuRhiContext::surfaceCompatibility() const noexcept {
    if (!isGpuBacked() || !m_impl || m_impl->backend != QRhi::Metal ||
        !m_impl->metalCommandQueueBound || !m_readbackFence) return {};
    const GpuFenceIdentity identity = m_readbackFence->identity();
    return {identity.deviceDomainId, identity.authorityEpoch};
}
std::shared_ptr<GpuFence> GpuRhiContext::createFence() const {
    const auto* nativeHandles = static_cast<const QRhiMetalNativeHandles*>(rhi->nativeHandles());
    if (!nativeHandles || !nativeHandles->cmdQueue) return nullptr;
    m_impl->metalCommandQueueBound = true;
    return makeMetalGpuFence(nativeHandles->cmdQueue, m_impl->deviceAuthorityEpoch);
}
"""
    audit_apple_rhi_surface_compatibility(safe, "safe Apple RHI binding mutation")
    for mutation, message in (
        (safe.replace("const GpuFenceIdentity identity",
                      "static const GpuFenceIdentity identity"),
         "cross-context cached identity must be rejected"),
        (safe.replace("!isGpuBacked() || ", ""),
         "Null and invalid QRhi backends must fail closed"),
        (safe.replace("m_impl->backend != QRhi::Metal ||\n        ", ""),
         "non-Metal QRhi backends must fail closed"),
        (safe.replace("!m_impl->metalCommandQueueBound || ", ""),
         "Metal contexts without native queue evidence must fail closed"),
        (safe.replace(" || !m_readbackFence", ""),
         "nil bound fence must fail closed"),
        (safe.replace("m_readbackFence->identity()", "GpuFence::create()->identity()"),
         "binding from a newly selected default device must be rejected"),
        (safe.replace("identity.deviceDomainId", "0"),
         "zero actual device domain must be rejected"),
        (safe.replace("identity.authorityEpoch", "currentDeviceAuthorityEpoch()"),
         "late global authority reads must be rejected"),
        (safe.replace("if (!nativeHandles || !nativeHandles->cmdQueue) return nullptr;", ""),
         "missing native Metal command queues must fail closed"),
        (safe.replace("m_impl->metalCommandQueueBound = true;", ""),
         "native Metal queue provenance must be recorded"),
    ):
        try:
            audit_apple_rhi_surface_compatibility(mutation, "Apple RHI binding mutation")
        except AssertionError:
            continue
        raise AssertionError(message)


def audit_apple_compositor_context_binding(source, label):
    alias = function_block(source, "std::shared_ptr<GpuSurface> aliasGpuNv12Surface")
    alias_declaration_start = strip_comments_and_literals(source).find(
        "std::shared_ptr<GpuSurface> aliasGpuNv12Surface")
    alias_open = strip_comments_and_literals(source).find("{", alias_declaration_start)
    alias_declaration = strip_comments_and_literals(source)[alias_declaration_start:alias_open]
    alias_binding = re.search(r"GpuSurfaceCompatibility\s+(\w+)", alias_declaration)
    require(alias_binding is not None and
            re.search(r"surface\s*->\s*compatibility\s*\(\s*\)", alias) and
            re.search(rf"{re.escape(alias_binding.group(1))}\s*\.\s*deviceDomainId", alias) and
            re.search(rf"{re.escape(alias_binding.group(1))}\s*\.\s*authorityEpoch", alias),
            f"{label}: aliases must reject surfaces bound to another context/device")

    upload = function_block(source, "std::shared_ptr<GpuSurface> uploadFrameToNv12Surface")
    upload_declaration_start = strip_comments_and_literals(source).find(
        "std::shared_ptr<GpuSurface> uploadFrameToNv12Surface")
    upload_open = strip_comments_and_literals(source).find("{", upload_declaration_start)
    upload_declaration = strip_comments_and_literals(source)[upload_declaration_start:upload_open]
    upload_binding = re.search(r"GpuSurfaceCompatibility\s+(\w+)", upload_declaration)
    require(upload_binding is not None and
            re.search(rf"aliasGpuNv12Surface\s*\([^;]*{re.escape(upload_binding.group(1))}",
                      upload) and
            re.search(rf"makeAppleNv12Surface\s*\([^;]*{re.escape(upload_binding.group(1))}",
                      upload),
            f"{label}: input aliases and uploads must consume the same context binding")

    for signature, downstream in (
            ("makeInputNv12Surface(", "uploadFrameToNv12Surface"),
            ("makeOutputRgba8Surface(", "makeAppleRgba8Surface")):
        factory = function_block(source, signature)
        require(re.search(r"if\s*\(\s*!\s*rhi\s*\)\s*return\s+nullptr\s*;", factory),
                f"{label}: {signature} must reject a nil context")
        binding = re.search(r"(?:const\s+)?GpuSurfaceCompatibility\s+(\w+)\s*=\s*"
                            r"rhi\s*->\s*surfaceCompatibility\s*\(\s*\)\s*;", factory)
        require(binding is not None and "static" not in [token for token, _ in tokens(factory)] and
                re.search(rf"{re.escape(binding.group(1))}\s*\.\s*deviceDomainId\s*==\s*0",
                          factory) and
                re.search(rf"{re.escape(binding.group(1))}\s*\.\s*authorityEpoch\s*==\s*0",
                          factory) and
                re.search(rf"{re.escape(downstream)}\s*\([^;]*"
                          rf"{re.escape(binding.group(1))}", factory),
                f"{label}: {signature} must bind each surface to its live context and fail closed")


def apple_compositor_context_binding_mutation_self_tests():
    safe = """
std::shared_ptr<GpuSurface> aliasGpuNv12Surface(
        const FrameHandle& frame, GpuSurfaceCompatibility compatibility) {
    auto surface = data->surfacePtr();
    const auto actual = surface->compatibility();
    if (actual.deviceDomainId != compatibility.deviceDomainId ||
        actual.authorityEpoch != compatibility.authorityEpoch) return nullptr;
    return surface;
}
std::shared_ptr<GpuSurface> uploadFrameToNv12Surface(
        const FrameHandle& frame, GpuSurfaceCompatibility compatibility) {
    if (auto aliased = aliasGpuNv12Surface(frame, compatibility)) return aliased;
    return makeAppleNv12Surface(width, height, compatibility);
}
std::shared_ptr<GpuSurface> makeInputNv12Surface(
        const FrameHandle& frame, const std::shared_ptr<GpuRhiContext>& rhi) {
    if (!rhi) return nullptr;
    const GpuSurfaceCompatibility compatibility = rhi->surfaceCompatibility();
    if (compatibility.deviceDomainId == 0 || compatibility.authorityEpoch == 0) return nullptr;
    return uploadFrameToNv12Surface(frame, compatibility);
}
std::shared_ptr<GpuSurface> makeOutputRgba8Surface(
        int width, int height, const std::shared_ptr<GpuRhiContext>& rhi) {
    if (!rhi) return nullptr;
    const GpuSurfaceCompatibility compatibility = rhi->surfaceCompatibility();
    if (compatibility.deviceDomainId == 0 || compatibility.authorityEpoch == 0) return nullptr;
    return makeAppleRgba8Surface(width, height, compatibility);
}
"""
    audit_apple_compositor_context_binding(safe, "safe Apple compositor binding mutation")
    for mutation, message in (
        (safe.replace("    if (!rhi) return nullptr;\n", "", 1),
         "nil compositor context must be rejected"),
        (safe.replace("const GpuSurfaceCompatibility compatibility =",
                      "static const GpuSurfaceCompatibility compatibility =", 1),
         "device-change-sensitive context binding must not be static"),
        (safe.replace("        actual.authorityEpoch != compatibility.authorityEpoch",
                      "        false"),
         "old-authority aliases must be rejected"),
        (safe.replace("makeAppleRgba8Surface(width, height, compatibility)",
                      "makeAppleRgba8Surface(width, height, {})"),
         "output factory must forward its actual context binding"),
    ):
        try:
            audit_apple_compositor_context_binding(mutation,
                                                   "Apple compositor binding mutation")
        except AssertionError:
            continue
        raise AssertionError(message)


def audit_apple_bound_wrap_calls(vt_importer, playback_worker, label):
    vt_import = function_block(vt_importer, "FrameHandle importVtImageBuffer")
    vt_binding = re.search(r"(?:const\s+)?GpuSurfaceCompatibility\s+(\w+)\s*=\s*"
                           r"rhi\s*->\s*surfaceCompatibility\s*\(\s*\)\s*;", vt_import)
    require(re.search(r"if\s*\(\s*!\s*rhi\s*\|\|\s*!\s*rhi\s*->\s*isGpuBacked\s*"
                      r"\(\s*\)\s*\)\s*return\s+FrameHandle\s*\(\s*\)\s*;",
                      vt_import) and vt_binding is not None and
            re.search(rf"{re.escape(vt_binding.group(1))}\s*\.\s*deviceDomainId\s*==\s*0",
                      vt_import) and
            re.search(rf"{re.escape(vt_binding.group(1))}\s*\.\s*authorityEpoch\s*==\s*0",
                      vt_import) and
            re.search(rf"wrapAppleImageBuffer\s*\(\s*cvImageBufferRef\s*,\s*"
                      rf"{re.escape(vt_binding.group(1))}\s*\)", vt_import),
            f"{label}: VideoToolbox import must bind wrappers to its actual RHI context")
    vt_surface = function_block(vt_importer, "FrameHandle importVtSurface")
    require(re.search(r"if\s*\([^;]*!\s*rhi\s*->\s*isGpuBacked\s*\(\s*\)[^;]*\)\s*"
                      r"return\s+FrameHandle\s*\(\s*\)\s*;", vt_surface) and
            re.search(r"expected\s*=\s*rhi\s*->\s*surfaceCompatibility\s*\(\s*\)",
                      vt_surface) and
            re.search(r"actual\s*=\s*surface\s*->\s*compatibility\s*\(\s*\)", vt_surface) and
            re.search(r"actual\s*\.\s*deviceDomainId\s*!=\s*expected\s*\.\s*deviceDomainId",
                      vt_surface) and
            re.search(r"actual\s*\.\s*authorityEpoch\s*!=\s*expected\s*\.\s*authorityEpoch",
                      vt_surface),
            f"{label}: pre-wrapped VT surfaces must match the receiving context exactly")
    decode = function_block(playback_worker, "int64_t PlaybackWorker::decodePacketIntoBank")
    require(re.search(r"wrapAppleImageBuffer\s*\(\s*imageBuffer\s*,\s*"
                      r"gpuRhi\s*->\s*surfaceCompatibility\s*\(\s*\)\s*\)", decode),
            f"{label}: playback decode must bind wrappers to the active RHI context")


def apple_bound_wrap_calls_mutation_self_tests():
    vt_safe = """
FrameHandle importVtImageBuffer(void* cvImageBufferRef, std::shared_ptr<GpuRhiContext> rhi) {
    if (!rhi || !rhi->isGpuBacked()) return FrameHandle();
    const GpuSurfaceCompatibility compatibility = rhi->surfaceCompatibility();
    if (compatibility.deviceDomainId == 0 || compatibility.authorityEpoch == 0)
        return FrameHandle();
    auto surface = wrapAppleImageBuffer(cvImageBufferRef, compatibility);
    return importVtSurface(surface);
}
FrameHandle importVtSurface(const std::shared_ptr<GpuSurface>& surface,
                            std::shared_ptr<GpuRhiContext> rhi) {
    if (!surface || !surface->isValid() || !rhi || !rhi->isGpuBacked()) return FrameHandle();
    const GpuSurfaceCompatibility expected = rhi->surfaceCompatibility();
    const GpuSurfaceCompatibility actual = surface->compatibility();
    if (actual.deviceDomainId != expected.deviceDomainId ||
        actual.authorityEpoch != expected.authorityEpoch) return FrameHandle();
    return FrameHandle();
}
"""
    playback_safe = """
int64_t PlaybackWorker::decodePacketIntoBank() {
    auto surface = wrapAppleImageBuffer(imageBuffer, gpuRhi->surfaceCompatibility());
    return surface ? 1 : 0;
}
"""
    audit_apple_bound_wrap_calls(vt_safe, playback_safe,
                                 "safe Apple decoded binding mutation")
    for mutation, message in (
        (vt_safe.replace("if (!rhi || !rhi->isGpuBacked())",
                         "if (!rhi)"),
         "VT image-buffer imports must reject Null QRhi contexts"),
        (vt_safe.replace("if (!surface || !surface->isValid() || !rhi || "
                         "!rhi->isGpuBacked())",
                         "if (!surface || !surface->isValid() || !rhi)"),
         "VT pre-wrapped imports must reject Null QRhi contexts"),
    ):
        try:
            audit_apple_bound_wrap_calls(mutation, playback_safe,
                                         "Apple decoded binding mutation")
        except AssertionError:
            continue
        raise AssertionError(message)


def audit_gpu_read_lease_snapshot_model(source, label):
    lease = function_block(source, "class GpuReadLease final")
    require("m_borrowedSurface" not in strip_comments_and_literals(lease),
            f"{label}: unreachable borrowed-surface state must not shadow retained snapshots")
    require(re.search(r"GpuSurfaceDesc\s+desc\s*\(\s*\)\s*const\s*\{\s*return\s+m_desc\s*;",
                      lease) and
            re.search(r"bool\s+valid\s*\(\s*\)\s*const\s*\{\s*return\s+m_valid\s*;",
                      lease) and
            re.search(r"uint32_t\s+nativeSubresource\s*\(\s*\)\s*const\s*\{\s*"
                      r"return\s+m_nativeSubresource\s*;", lease),
            f"{label}: metadata access must use the immutable acquisition snapshot")
    require(re.search(r"return\s+active\s*\?\s*m_nativeHandle\s*:\s*nullptr\s*;", lease),
            f"{label}: gated native access must use the owner-backed handle snapshot")
    require(re.search(r"m_surfaceOwner\s*\(\s*accessAuthorized\s*\?\s*surface", lease) and
            re.search(r"m_nativeHandle\s*\(\s*accessAuthorized\s*&&\s*surface\s*\?\s*"
                      r"surface\s*->\s*nativeHandle\s*\(\s*\)", lease),
            f"{label}: shared reads must retain the exact surface backing their handle snapshot")
    require(re.search(r"m_nativeOwner\s*\([^)]*surface\s*->\s*retainNativeHandle\s*\(\s*\)",
                      lease) and
            re.search(r"m_nativeHandle\s*\(\s*m_nativeOwner\s*\.\s*get\s*\(\s*\)\s*\)",
                      lease),
            f"{label}: raw encoder reads must retain native backing before exposing its snapshot")


def gpu_read_lease_snapshot_model_mutation_self_tests():
    safe = """
class GpuReadLease final {
public:
    GpuSurfaceDesc desc() const { return m_desc; }
    bool valid() const { return m_valid; }
    void* nativeHandle() const {
        const bool active = m_accessAuthorized;
        return active ? m_nativeHandle : nullptr;
    }
    uint32_t nativeSubresource() const { return m_nativeSubresource; }
private:
    GpuReadLease(const std::shared_ptr<GpuSurface>& surface, bool accessAuthorized)
        : m_surfaceOwner(accessAuthorized ? surface : std::shared_ptr<GpuSurface>{}),
          m_nativeHandle(accessAuthorized && surface ? surface->nativeHandle() : nullptr) {}
    GpuReadLease(GpuSurface* surface, bool accessAuthorized)
        : m_nativeOwner(accessAuthorized && surface ? surface->retainNativeHandle()
                                                    : GpuOwnedNativeHandle{}),
          m_nativeHandle(m_nativeOwner.get()) {}
    GpuSurfaceDesc m_desc;
    bool m_valid = false;
    std::shared_ptr<GpuSurface> m_surfaceOwner;
    GpuOwnedNativeHandle m_nativeOwner;
    void* m_nativeHandle = nullptr;
    uint32_t m_nativeSubresource = 0;
    bool m_accessAuthorized = false;
};
"""
    audit_gpu_read_lease_snapshot_model(safe, "safe read lease snapshot mutation")
    for mutation, message in (
        (safe.replace("    GpuSurfaceDesc m_desc;",
                      "    GpuSurface* m_borrowedSurface = nullptr;\n    GpuSurfaceDesc m_desc;"),
         "dead borrowed-surface state must be rejected"),
        (safe.replace("return m_desc;", "return m_borrowedSurface->desc();"),
         "metadata re-reads through a raw surface must be rejected"),
        (safe.replace("return active ? m_nativeHandle : nullptr;",
                      "return active ? m_borrowedSurface->nativeHandle() : nullptr;"),
         "native handle re-reads through a raw surface must be rejected"),
        (safe.replace("m_surfaceOwner(accessAuthorized ? surface : std::shared_ptr<GpuSurface>{})",
                      "m_surfaceOwner()"),
         "shared reads must retain the surface owner"),
        (safe.replace("m_nativeHandle(m_nativeOwner.get())",
                      "m_nativeHandle(surface->nativeHandle())"),
         "raw reads must expose the retained native owner rather than a borrowed surface"),
    ):
        try:
            audit_gpu_read_lease_snapshot_model(mutation, "read lease snapshot mutation")
        except AssertionError:
            continue
        raise AssertionError(message)


def audit_imported_nv12_backing_lifetime(source, label):
    render = function_block(source, "RenderGridResult renderGridWithRhi")
    imported_owners = re.search(
        r"std\s*::\s*vector\s*<\s*std\s*::\s*unique_ptr\s*<\s*"
        r"gpucompositor\s*::\s*ImportedNv12Source\s*>\s*>\s*importedSources\s*;",
        render,
    )
    luma_owners = re.search(
        r"std\s*::\s*vector\s*<\s*std\s*::\s*unique_ptr\s*<\s*QRhiTexture\s*>\s*>\s*"
        r"ownedLumaTextures\s*;",
        render,
    )
    chroma_owners = re.search(
        r"std\s*::\s*vector\s*<\s*std\s*::\s*unique_ptr\s*<\s*QRhiTexture\s*>\s*>\s*"
        r"ownedChromaTextures\s*;",
        render,
    )
    require(imported_owners is not None and luma_owners is not None and
            chroma_owners is not None and
            imported_owners.start() < luma_owners.start() < chroma_owners.start(),
            f"{label}: imported backing vector must outlive both QRhi wrapper vectors")

    native_branch = function_block(
        render, "if (source.present && source.nativeSlot != std::numeric_limits<size_t>::max())")
    imported = re.search(
        r"auto\s+imported\s*=\s*[^;]*importNv12Source\s*\([^;]*;", native_branch)
    y_texture = re.search(r"std\s*::\s*unique_ptr\s*<\s*QRhiTexture\s*>\s+yTex\s*\(",
                          native_branch)
    uv_texture = re.search(r"std\s*::\s*unique_ptr\s*<\s*QRhiTexture\s*>\s+uvTex\s*\(",
                           native_branch)
    require(imported is not None and y_texture is not None and uv_texture is not None and
            imported.start() < y_texture.start() < uv_texture.start(),
            f"{label}: per-source imported backing must outlive wrappers on partial failure")
    luma_create = re.search(r"yTex\s*->\s*createFrom\s*\(", native_branch)
    chroma_create = re.search(r"uvTex\s*->\s*createFrom\s*\(", native_branch)
    retained = re.search(r"importedSources\s*\.\s*push_back\s*\(\s*std\s*::\s*move\s*\(\s*"
                         r"imported\s*\)\s*\)", native_branch)
    require(luma_create is not None and chroma_create is not None and retained is not None and
            uv_texture.end() < luma_create.start() < chroma_create.start() < retained.start(),
            f"{label}: backing must stay local until both QRhi wrappers are created")


def imported_nv12_backing_lifetime_mutation_self_tests():
    safe = """
RenderGridResult renderGridWithRhi() {
    std::vector<std::unique_ptr<gpucompositor::ImportedNv12Source>> importedSources;
    std::vector<std::unique_ptr<QRhiTexture>> ownedLumaTextures;
    std::vector<std::unique_ptr<QRhiTexture>> ownedChromaTextures;
    if (source.present && source.nativeSlot != std::numeric_limits<size_t>::max()) {
        auto imported = gpucompositor::importNv12Source(rhi, *nativeSource);
        std::unique_ptr<QRhiTexture> yTex(rhi->newTexture());
        std::unique_ptr<QRhiTexture> uvTex(rhi->newTexture());
        if (!yTex->createFrom(imported->lumaNativeTexture()) ||
            !uvTex->createFrom(imported->chromaNativeTexture())) return {};
        importedSources.push_back(std::move(imported));
    }
}
"""
    audit_imported_nv12_backing_lifetime(safe, "safe imported NV12 lifetime mutation")
    normal_release_first = safe.replace(
        "    std::vector<std::unique_ptr<gpucompositor::ImportedNv12Source>> importedSources;\n",
        "",
    ).replace(
        "    std::vector<std::unique_ptr<QRhiTexture>> ownedChromaTextures;\n",
        "    std::vector<std::unique_ptr<QRhiTexture>> ownedChromaTextures;\n"
        "    std::vector<std::unique_ptr<gpucompositor::ImportedNv12Source>> importedSources;\n",
    )
    partial_release_first = safe.replace(
        "        auto imported = gpucompositor::importNv12Source(rhi, *nativeSource);\n",
        "",
    ).replace(
        "        std::unique_ptr<QRhiTexture> uvTex(rhi->newTexture());\n",
        "        std::unique_ptr<QRhiTexture> uvTex(rhi->newTexture());\n"
        "        auto imported = gpucompositor::importNv12Source(rhi, *nativeSource);\n",
    )
    for mutation, message in (
        (normal_release_first, "normal imported-backing early release must be rejected"),
        (partial_release_first, "partial-failure imported-backing early release must be rejected"),
    ):
        try:
            audit_imported_nv12_backing_lifetime(mutation, "imported NV12 lifetime mutation")
        except AssertionError:
            continue
        raise AssertionError(message)


def audit_apple_scoped_wrapper_pixel_format(source, label):
    mapping = function_block(source, "OSType expectedCvPixelFormatFor")
    require(re.search(
        r"case\s+FramePixelFormat\s*::\s*Nv12\s*:\s*return\s+"
        r"kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange\s*;",
        mapping,
    ) is not None,
            f"{label}: NV12 wrappers must require the video-range bi-planar format")
    require(re.search(
        r"case\s+FramePixelFormat\s*::\s*Rgba8\s*:\s*return\s+"
        r"kCVPixelFormatType_32BGRA\s*;",
        mapping,
    ) is not None,
            f"{label}: RGBA wrappers must require the BGRA IOSurface format")
    require(re.search(r"default\s*:\s*return\s+0\s*;", mapping) is not None,
            f"{label}: unsupported frame formats must fail closed")

    wrapper = function_block(
        source, "CVPixelBufferRef retainApplePixelBufferWrapper(const GpuScopedNativeSurface&")
    require(re.search(
        r"const\s+OSType\s+expectedPixelFormat\s*=\s*"
        r"expectedCvPixelFormatFor\s*\(\s*desc\s*\.\s*format\s*\)\s*;",
        wrapper,
    ) is not None,
            f"{label}: scoped wrapper must derive its expected format from the scoped descriptor")
    require(re.search(r"if\s*\(\s*!\s*expectedPixelFormat\s*\)\s*return\s+nullptr\s*;",
                      wrapper) is not None,
            f"{label}: unsupported scoped formats must be rejected before wrapper creation")
    require(re.search(
        r"CVPixelBufferGetPixelFormatType\s*\(\s*result\s*\)\s*!=\s*"
        r"expectedPixelFormat",
        wrapper,
    ) is not None,
            f"{label}: the created wrapper must match the exact expected CoreVideo format")


def apple_scoped_wrapper_pixel_format_mutation_self_tests():
    safe = """
OSType expectedCvPixelFormatFor(FramePixelFormat format) {
    switch (format) {
    case FramePixelFormat::Nv12:
        return kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    case FramePixelFormat::Rgba8:
        return kCVPixelFormatType_32BGRA;
    default:
        return 0;
    }
}
CVPixelBufferRef retainApplePixelBufferWrapper(const GpuScopedNativeSurface& surface) {
    const GpuSurfaceDesc desc = surface.desc();
    const OSType expectedPixelFormat = expectedCvPixelFormatFor(desc.format);
    if (!expectedPixelFormat) return nullptr;
    CVPixelBufferRef result = nullptr;
    if (CVPixelBufferGetPixelFormatType(result) != expectedPixelFormat) return nullptr;
    return result;
}
"""
    audit_apple_scoped_wrapper_pixel_format(safe, "safe Apple scoped wrapper mutation")
    for mutation, message in (
        (safe.replace("kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange",
                      "kCVPixelFormatType_420YpCbCr8BiPlanarFullRange"),
         "an NV12 full-range mismatch must be rejected"),
        (safe.replace("kCVPixelFormatType_32BGRA", "kCVPixelFormatType_32RGBA"),
         "an RGBA channel-order mismatch must be rejected"),
        (safe.replace("    if (!expectedPixelFormat) return nullptr;\n", ""),
         "an unsupported Apple wrapper format must fail before creation"),
        (safe.replace("expectedCvPixelFormatFor(desc.format)",
                      "expectedCvPixelFormatFor(globalFormat)"),
         "Apple wrapper validation must use the exact scoped descriptor"),
    ):
        try:
            audit_apple_scoped_wrapper_pixel_format(mutation,
                                                    "Apple scoped wrapper mutation")
        except AssertionError:
            continue
        raise AssertionError(message)


RAW_READBACK_CALL_RE = re.compile(
    r"(?:->|\.)\s*importAndReadback\s*\(",
    re.DOTALL,
)


def production_condition_possibilities(keyword, expression):
    """Conservatively model a conditional with OLR_UNIT_TEST undefined."""
    expression = re.sub(r"\s+", "", expression)
    if "OLR_UNIT_TEST" not in expression:
        return True, True
    if keyword == "ifdef":
        return (False, True) if expression == "OLR_UNIT_TEST" else (True, True)
    if keyword == "ifndef":
        return (True, False) if expression == "OLR_UNIT_TEST" else (True, True)

    positive_atom = r"(?:defined\(OLR_UNIT_TEST\)|OLR_UNIT_TEST)"
    if re.fullmatch(positive_atom, expression) is not None:
        return False, True
    if re.fullmatch(rf"(?:{positive_atom})==1|1==(?:{positive_atom})", expression) is not None:
        return False, True
    if re.fullmatch(
            rf"!(?:{positive_atom})|!\((?:{positive_atom})\)|"
            rf"(?:{positive_atom})==0|0==(?:{positive_atom})",
            expression) is not None:
        return True, False

    # Do not infer anything from a sub-expression without parsing the complete
    # Boolean grammar: `UNIT && X || !UNIT` is active in production. Retaining
    # an unrecognized branch may cause a false positive, but can never hide a
    # production raw-readback call.
    return True, True


def production_preprocessor_view(source):
    """Blank only branches that cannot exist when OLR_UNIT_TEST is undefined."""
    output = []
    active = True
    stack = []
    for line in source.splitlines(keepends=True):
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if directive is None:
            output.append(line if active else "\n" if line.endswith("\n") else "")
            continue

        keyword, expression = directive.group(1), directive.group(2)
        if keyword in ("if", "ifdef", "ifndef"):
            parent_active = active
            can_be_true, can_be_false = production_condition_possibilities(keyword, expression)
            stack.append([parent_active, can_be_false])
            active = parent_active and can_be_true
        elif keyword == "else" and stack:
            parent_active, remaining_possible = stack[-1]
            active = parent_active and remaining_possible
            stack[-1][1] = False
        elif keyword == "elif" and stack:
            parent_active, remaining_possible = stack[-1]
            can_be_true, can_be_false = production_condition_possibilities("if", expression)
            active = parent_active and remaining_possible and can_be_true
            stack[-1][1] = remaining_possible and can_be_false
        elif keyword == "endif" and stack:
            parent_active, _ = stack.pop()
            active = parent_active
        output.append(line if active else "\n" if line.endswith("\n") else "")
    require(not stack, "unterminated preprocessor conditional")
    return "".join(output)


def audit_scoped_readback_caller(source, label, require_helper=False):
    cleaned = strip_comments_and_literals(source)
    require(RAW_READBACK_CALL_RE.search(cleaned) is None,
            f"{label}: direct importAndReadback call bypasses retirement preparation")
    if require_helper:
        helper_items = tokens(cleaned)
        helper_tokens = [token for token, _ in helper_items]
        helper_braces = token_pairs(helper_items, "{", "}")
        helper_calls = [index for index, token in enumerate(helper_tokens)
                        if token == "submitGpuReadback"]
        require(helper_calls,
                f"{label}: scoped submitGpuReadback helper is required")
        outer_body = next((index for index, token in enumerate(helper_tokens)
                           if token == "{"), None)
        require(outer_body is not None,
                f"{label}: scoped submitGpuReadback helper has no callable body")
        def preceded_by_return(index):
            prefix = cleaned[:helper_items[index][1]]
            previous_statement_end = prefix.rfind(";")
            if previous_statement_end < 0:
                return False
            completed = prefix[:previous_statement_end + 1]
            return re.search(r"\breturn\b[^;{}]*(?:\{[^{}]*\}[^;{}]*)?;\s*$",
                             completed, re.DOTALL) is not None

        require(any(not is_obviously_unreachable(helper_items, index, helper_braces) and
                    not preceded_by_return(index) and
                    direct_enclosing_brace(helper_braces, index) == outer_body
                    for index in helper_calls),
                f"{label}: scoped submitGpuReadback helper is unreachable")


def audit_scoped_readback_helper(frame_data, label):
    cleaned = strip_comments_and_literals(frame_data)
    helper = function_block(frame_data, "GpuReadbackResult submitGpuReadback")
    helper_items = tokens(helper)
    helper_tokens = [token for token, _ in helper_items]
    helper_braces = token_pairs(helper_items, "{", "}")
    helper_parens = token_pairs(helper_items, "(", ")")
    require(len(RAW_READBACK_CALL_RE.findall(helper)) == 1,
            f"{label}: helper must own exactly one raw readback call")
    require(len(RAW_READBACK_CALL_RE.findall(cleaned)) == 1,
            f"{label}: raw readback calls outside the helper are forbidden")
    for required in ("GpuOpScope", "submit", "outcome"):
        require(required in helper_tokens,
                f"{label}: helper is missing required scoped outcome token {required}")
    scope_candidates = [
        index for index in range(len(helper_items) - 1)
        if helper_tokens[index:index + 2] == ["GpuOpScope", "operation"]
    ]
    submit_candidates = [
        index for index in range(2, len(helper_items) - 1)
        if helper_tokens[index - 2:index + 2] == ["operation", ".", "submit", "("]
    ]
    require(len(scope_candidates) == 1,
            f"{label}: helper must construct exactly one named GpuOpScope operation")
    require(len(submit_candidates) == 1,
            f"{label}: helper must submit exactly once through that GpuOpScope operation")
    scope_index = scope_candidates[0]
    submit_index = submit_candidates[0]
    raw_index = helper_tokens.index("importAndReadback")
    require(not is_obviously_unreachable(helper_items, scope_index, helper_braces),
            f"{label}: GpuOpScope construction is unreachable")
    require(not is_obviously_unreachable(helper_items, submit_index, helper_braces),
            f"{label}: GpuOpScope submission is unreachable")
    require(not is_obviously_unreachable(helper_items, raw_index, helper_braces),
            f"{label}: raw readback callback is unreachable")

    adapter_declaration = next(
        (index for index in range(len(helper_items) - 2)
         if helper_tokens[index:index + 3] == ["auto", "adapter", "="]),
        None,
    )
    require(adapter_declaration is not None and scope_index < adapter_declaration < submit_index,
            f"{label}: readback adapter must be prepared by the live GpuOpScope path")
    adapter_open = next(
        (index for index in range(adapter_declaration + 3, submit_index)
         if helper_tokens[index] == "{"),
        None,
    )
    require(adapter_open is not None and adapter_open < raw_index < helper_braces[adapter_open],
            f"{label}: raw readback must execute inside the submitted adapter")
    require(direct_enclosing_brace(helper_braces, raw_index) == adapter_open,
            f"{label}: raw readback must execute directly in the submitted adapter")
    submit_open = submit_index + 1
    require("adapter" in helper_tokens[submit_open + 1:helper_parens[submit_open]],
            f"{label}: GpuOpScope must submit the readback adapter")
    definition = re.search(r"GpuReadbackResult\s+submitGpuReadback\s*\((?P<args>[^)]*)\)",
                           cleaned, re.DOTALL)
    require(definition is not None and "GpuFence" not in definition.group("args"),
            f"{label}: callers must not provide a readback fence")


def audit_apple_readback_fallback(playback_worker, label):
    function = function_block(playback_worker, "int64_t PlaybackWorker::decodePacketIntoBank")
    cleaned = strip_comments_and_literals(function)
    surface = cleaned.find("wrapAppleImageBuffer")
    fallback = cleaned.find("auto cpuFallback", surface)
    fallback_open = cleaned.find("{", fallback)
    require(surface >= 0 and fallback > surface and fallback_open > fallback,
            f"{label}: could not isolate Apple CPU fallback")
    fallback_close = matching_character(cleaned, fallback_open, "{", "}")
    fallback_body = cleaned[fallback_open:fallback_close + 1]
    audit_scoped_readback_caller(fallback_body, label, True)
    require(re.search(r"if\s*\(\s*false\s*\)[^{;]*submitGpuReadback", fallback_body) is None,
            f"{label}: scoped helper is obviously unreachable")
    windows_surface = cleaned.find("m_winGpuImportEdge->tryImportSurface", fallback_close)
    apple_region_end = windows_surface if windows_surface >= 0 else len(cleaned)
    apple_region = cleaned[fallback_close + 1:apple_region_end]
    handle_surface = cleaned.rfind("auto handleSurface", 0, fallback_open)
    handle_open = cleaned.find("{", handle_surface)
    require(handle_surface >= 0 and handle_open > handle_surface,
            f"{label}: could not isolate the Apple handleSurface callback")
    handle_close = matching_character(cleaned, handle_open, "{", "}")
    calls = list(re.finditer(r"\bmintGpuOrDegrade\s*\(", apple_region))
    mint_assignments = list(re.finditer(r"\bGpuMintResult\s+mint\s*=", apple_region))
    assignments = list(re.finditer(
        r"\bGpuMintResult\s+mint\s*=\s*mintGpuOrDegrade\s*\(", apple_region))
    require(len(calls) == 1 and len(mint_assignments) == 1 and len(assignments) == 1,
            f"{label}: Apple path must contain exactly one assigned mintGpuOrDegrade call")
    assignment = assignments[0]
    require(calls[0].start() == assignment.group(0).find("mintGpuOrDegrade") + assignment.start(),
            f"{label}: the actual Apple mint assignment must own the sole call")
    prefix = apple_region[:assignment.start()]
    require(re.search(r"(?:if(?:\s+constexpr)?|while)\s*\(\s*(?:false|0)\s*\)\s*"
                      r"(?:\{\s*)?$", prefix) is None,
            f"{label}: Apple mint assignment is obviously unreachable")
    apple_items = tokens(cleaned)
    apple_braces = token_pairs(apple_items, "{", "}")
    assignment_position = fallback_close + 1 + assignment.start()
    assignment_token = next(
        (index for index, (token, position) in enumerate(apple_items)
         if token == "GpuMintResult" and position >= assignment_position),
        None,
    )
    require(assignment_token is not None and
            not is_obviously_unreachable(apple_items, assignment_token, apple_braces),
            f"{label}: Apple mint assignment is unreachable")
    handle_body_token = next(
        (index for index, (token, position) in enumerate(apple_items)
         if token == "{" and position == handle_open),
        None,
    )
    enclosing_lambda_bodies = [
        body_open for body_open in lambda_body_openings(apple_items, apple_braces)
        if body_open < assignment_token < apple_braces[body_open]
    ]
    require(handle_body_token is not None and enclosing_lambda_bodies == [handle_body_token] and
            direct_enclosing_brace(apple_braces, assignment_token) == handle_body_token,
            f"{label}: Apple mint must execute directly in handleSurface")
    call = calls[0].start()
    call_open = apple_region.find("(", call)
    call_close = matching_character(apple_region, call_open, "(", ")")
    call_arguments = apple_region[call_open + 1:call_close]
    require(re.search(r",\s*cpuFallback\s*$", call_arguments, re.DOTALL) is not None,
            f"{label}: named cpuFallback must be passed to mintGpuOrDegrade")

    post_handle = cleaned[handle_close + 1:apple_region_end]
    decode_calls = list(re.finditer(r"\bdecodeKeepSurface\s*\(", post_handle))
    require(len(decode_calls) == 1,
            f"{label}: handleSurface must have exactly one decodeKeepSurface consumer")
    decode_call_position = handle_close + 1 + decode_calls[0].start()
    decode_call_token = next(
        (index for index, (token, position) in enumerate(apple_items)
         if token == "decodeKeepSurface" and position >= decode_call_position),
        None,
    )
    require(decode_call_token is not None and
            not is_obviously_unreachable(apple_items, decode_call_token, apple_braces) and
            not is_inside_nested_lambda(apple_items, apple_braces, decode_call_token),
            f"{label}: decodeKeepSurface consumer is unreachable")
    decode_open = post_handle.find("(", decode_calls[0].start())
    decode_close = matching_character(post_handle, decode_open, "(", ")")
    decode_arguments = post_handle[decode_open + 1:decode_close]
    require(re.search(r"(?:^|,)\s*handleSurface\s*(?:,|$)", decode_arguments, re.DOTALL)
            is not None,
            f"{label}: reachable decodeKeepSurface must consume handleSurface")


def audit_production_raw_readback_calls(repo_root, helper_path):
    helper_resolved = helper_path.resolve()
    for source_path in first_party_source_paths(repo_root):
        source = source_path.read_text(encoding="utf-8")
        production = production_preprocessor_view(source)
        count = len(RAW_READBACK_CALL_RE.findall(strip_comments_and_literals(production)))
        if source_path.resolve() == helper_resolved:
            require(count == 1, "scoped helper must remain the sole production raw readback caller")
        else:
            require(count == 0,
                    f"direct production raw readback is forbidden: {source_path}")


def definition_with_signature(source, signature):
    cleaned = strip_comments_and_literals(source)
    signature_pattern = r"\s+".join(re.escape(part) for part in signature.split())
    match = re.search(signature_pattern, cleaned)
    require(match is not None, f"could not locate function {signature!r}")
    start = match.start()
    opening = cleaned.find("{", start)
    require(opening >= 0, f"could not locate body for {signature!r}")
    closing = matching_character(cleaned, opening, "{", "}")
    return cleaned[start:closing + 1]


def adapter_definition(function, label):
    match = re.search(
        r"\bauto\s+adapter\s*=\s*\[[^]]*\]\s*"
        r"\(\s*const\s+(?P<type>GpuScopedNativeView\s*<\s*1\s*>|auto)\s*&\s*"
        r"(?P<name>[A-Za-z_]\w*)\s*\)\s*noexcept\s*\{",
        function,
    )
    require(match is not None, f"{label}: adapter must accept one const scoped native view")
    body_open = function.find("{", match.start())
    body_close = matching_character(function, body_open, "{", "}")
    return match, function[body_open:body_close + 1]


def audit_exact_readback_owner_binding(frame_data, label):
    audit_scoped_readback_helper(frame_data, label)
    helper = function_block(frame_data, "GpuReadbackResult submitGpuReadback")
    adapter, body = adapter_definition(helper, label)
    require(re.sub(r"\s+", "", adapter.group("type")) == "GpuScopedNativeView<1>",
            f"{label}: readback adapter must name the exact one-slot scoped view")
    view = adapter.group("name")
    body_tokens = [token for token, _ in tokens(body)]
    require("surface" not in body_tokens and "GpuSurface" not in body_tokens,
            f"{label}: readback adapter must not recover or capture a retained owner")
    require("nativeHandle" not in body_tokens,
            f"{label}: readback adapter must not extract a raw native handle")
    require(len(lambda_body_openings(tokens(body), token_pairs(tokens(body), "{", "}"))) == 0,
            f"{label}: readback backend call must not hide in a deferred nested lambda")
    exact_call = re.compile(
        rf"\brhi\s*->\s*importAndReadback\s*\(\s*{re.escape(view)}\s*\.\s*get\s*"
        r"<\s*0\s*>\s*\(\s*\)\s*,\s*target\s*\)",
        re.DOTALL,
    )
    require(len(exact_call.findall(body)) == 1,
            f"{label}: raw readback must consume slot zero from the submitted scoped view")
    require(body_tokens.count(view) == 1,
            f"{label}: scoped readback view must be consumed exactly once and never escape")


def audit_exact_compositor_owner_binding(function, label):
    adapter, body = adapter_definition(function, label)
    require(adapter.group("type") == "auto",
            f"{label}: compacted compositor adapter must accept the submitted scoped view")
    view = adapter.group("name")
    body_items = tokens(body)
    body_tokens = [token for token, _ in body_items]
    body_braces = token_pairs(body_items, "{", "}")
    require("nativeHandle" not in body_tokens,
            f"{label}: compositor adapter must not extract a raw native handle")
    require(not any(token in body_tokens for token in
                    ("surface", "capturedSurface", "capturedNativeSurface", "sharedSurface",
                     "globalSurface", "GpuSurface", "GpuSyncReadScope", "GpuReadLease")),
            f"{label}: compositor adapter must not recover a captured/shared surface")
    require(len(lambda_body_openings(body_items, body_braces)) == 2,
            f"{label}: adapter may contain only the invoked render job and its scoped accessor")
    accessor = re.compile(
        rf"\bauto\s+nativeAt\s*=\s*\[\s*&\s*\]\s*\(\s*size_t\s+slot\s*\)\s*"
        rf"->\s*const\s+GpuScopedNativeSurface\s*\*\s*\{{\s*return\s+slot\s*<\s*"
        rf"{re.escape(view)}\s*\.\s*size\s*\(\s*\)\s*\?\s*&\s*"
        rf"{re.escape(view)}\s*\[\s*slot\s*\]\s*:\s*nullptr\s*;\s*\}}\s*;",
        re.DOTALL,
    )
    require(len(accessor.findall(body)) == 1,
            f"{label}: nativeAt must map every slot directly to the submitted scoped view")
    require(body_tokens.count(view) == 2,
            f"{label}: scoped compositor view must remain confined to its exact slot accessor")
    require(len(re.findall(r"\binvokeOnRenderThread\s*\(", body)) == 1,
            f"{label}: compositor adapter must own exactly one synchronous render dispatch")
    render_calls = list(re.finditer(r"\brenderGridWithRhi\s*\(", body))
    require(len(render_calls) == 1,
            f"{label}: compositor adapter must have exactly one real render primitive call")
    render_open = body.find("(", render_calls[0].start())
    render_close = matching_character(body, render_open, "(", ")")
    render_arguments = body[render_open + 1:render_close]
    require(re.search(r"(?:^|,)\s*nativeAt\s*(?:,|$)", render_arguments, re.DOTALL) is not None,
            f"{label}: render primitive must consume the accessor derived from the scoped view")
    render_index = next(index for index, (token, _) in enumerate(body_items)
                        if token == "renderGridWithRhi")
    require(not is_obviously_unreachable(body_items, render_index, body_braces),
            f"{label}: scoped render primitive is unreachable")
    require(re.search(r"\bsubmitCompactedOwners\s*<[^;]*\(\s*operation\s*,\s*adapter\s*,",
                      function, re.DOTALL) is not None,
            f"{label}: exact compositor adapter must be submitted with the retained pack")


def audit_scoped_surface_backend(source, signature, label, consumers=(),
                                 allow_direct_native_handle=False, allow_unused=False):
    definition = definition_with_signature(production_preprocessor_view(source), signature)
    header = definition[:definition.find("{")]
    body = definition[definition.find("{"):]
    require(re.search(r"const\s+GpuScopedNativeSurface\s*&\s*surface\b", header) is not None,
            f"{label}: backend primitive must accept the scoped native surface")
    body_items = tokens(body)
    body_tokens = [token for token, _ in body_items]
    body_braces = token_pairs(body_items, "{", "}")
    require(not any(token in body_tokens for token in
                    ("GpuSyncReadScope", "GpuReadLease", "owners", "globalSurface",
                     "sharedSurface", "capturedSurface", "capturedNativeSurface")),
            f"{label}: backend primitive must not recover an independent surface owner")
    native_surface_names = {"surface", "ioSurface", "IOSurfaceRef", "GpuScopedNativeSurface"}
    require(not any(token not in native_surface_names and "iosurface" not in token.lower() and
                    token.lower().endswith("surface")
                    for token in body_tokens),
            f"{label}: backend primitive must not name any surface except its scoped parameter")
    require(not any(body_tokens[index:index + 4] == ["shared_ptr", "<", "GpuSurface", ">"]
                    for index in range(max(0, len(body_tokens) - 3))),
            f"{label}: backend primitive must not recover a shared GpuSurface")
    surface_indices = [index for index, token in enumerate(body_tokens) if token == "surface"]
    require(allow_unused or surface_indices,
            f"{label}: backend primitive must consume its scoped native surface")
    require(all(not is_inside_nested_lambda(body_items, body_braces, index)
                for index in surface_indices),
            f"{label}: scoped surface must not be captured by a deferred nested lambda")
    require(re.search(r"&\s*\(*\s*surface\b", body) is None,
            f"{label}: scoped surface address must not escape the backend call")
    require(re.search(
        r"\b(?:auto|GpuScopedNativeSurface)\s*(?:const\s*)?&{1,2}\s*"
        r"[A-Za-z_]\w*\s*=\s*"
        r"\(*\s*surface\b",
        body,
    ) is None,
            f"{label}: scoped surface must not be rebound to an escaping reference")
    require(re.search(r"\bstd\s*::\s*(?:ref|cref)\s*\(\s*\(*\s*surface\b", body) is None,
            f"{label}: scoped surface must not escape through a reference wrapper")
    require(re.search(r"\breference_wrapper\b[^;]*\bsurface\b", body, re.DOTALL) is None,
            f"{label}: scoped surface must not escape through std::reference_wrapper")
    require(re.search(r"\[[^\]]*\bsurface\b[^\]]*\]", body, re.DOTALL) is None,
            f"{label}: scoped surface must not appear in a lambda capture")
    require(re.search(r"\breturn\s+\(*\s*(?:&\s*)?surface\b", body) is None,
            f"{label}: scoped surface must not be returned from the backend call")
    for index in range(len(body_tokens) - 2):
        require(body_tokens[index:index + 3] not in (["=", "&", "surface"],
                                                      ["return", "&", "surface"]),
                f"{label}: scoped surface reference must not escape the backend call")
    native_calls = list(re.finditer(r"\bnativeHandle\s*\(", body))
    if native_calls:
        require(allow_direct_native_handle and
                len(re.findall(r"\bsurface\s*\.\s*nativeHandle\s*\(\s*\)", body)) ==
                len(native_calls),
                f"{label}: only the scoped parameter may supply a raw native handle")
        require(re.search(r"(?:escaped|global|shared)\w*\s*=\s*[^;]*nativeHandle", body,
                          re.IGNORECASE) is None,
                f"{label}: raw native handle must not escape the backend primitive")
        for native_call in native_calls:
            statement_start = max(body.rfind(";", 0, native_call.start()),
                                  body.rfind("{", 0, native_call.start())) + 1
            statement_end = body.find(";", native_call.end())
            require(statement_end >= 0, f"{label}: raw native handle use must be bounded")
            statement = body[statement_start:statement_end + 1]
            declaration = re.search(
                r"(?:\bvoid\s*\*|\bIOSurfaceRef)\s+[A-Za-z_]\w*\s*=\s*"
                r"(?:static_cast\s*<[^>]+>\s*\(\s*)?surface\s*\.\s*nativeHandle\s*"
                r"\(\s*\)\s*\)?\s*;",
                statement,
                re.DOTALL,
            )
            require(declaration is not None,
                    f"{label}: raw native handle must stay in a function-local declaration")
            alias_match = re.search(
                r"(?:\bvoid\s*\*|\bIOSurfaceRef)\s+([A-Za-z_]\w*)\s*=", statement)
            require(alias_match is not None,
                    f"{label}: raw native handle alias could not be isolated")
            alias = alias_match.group(1)
            alias_indices = [index for index, token in enumerate(body_tokens) if token == alias]
            require(alias_indices and all(
                not is_inside_nested_lambda(body_items, body_braces, index)
                for index in alias_indices),
                    f"{label}: raw native handle must not enter a deferred callback")
            require(re.search(rf"\breturn\s+{re.escape(alias)}\b", body) is None,
                    f"{label}: raw native handle must not be returned")
    for consumer in consumers:
        calls = list(re.finditer(rf"\b{re.escape(consumer)}\s*\(", body))
        require(len(calls) == 1,
                f"{label}: expected exactly one {consumer} call")
        call_open = body.find("(", calls[0].start())
        call_close = matching_character(body, call_open, "(", ")")
        arguments = body[call_open + 1:call_close]
        require(re.fullmatch(r"\s*surface\s*", arguments, re.DOTALL) is not None,
                f"{label}: {consumer} must consume only the exact scoped parameter")
        call_index = next(index for index, (token, position) in enumerate(body_items)
                          if token == consumer and position >= calls[0].start())
        require(not is_obviously_unreachable(body_items, call_index, body_braces) and
                not is_inside_nested_lambda(body_items, body_braces, call_index) and
                not is_inside_conditional_block(body_items, body_braces, call_index),
                f"{label}: scoped backend consumer must execute directly and be reachable")
        statement_start = max(body.rfind(";", 0, calls[0].start()),
                              body.rfind("{", 0, calls[0].start())) + 1
        require(re.search(r"\b(?:if|while)\s*\([^;{}]*$", body[statement_start:calls[0].start()])
                is None,
                f"{label}: scoped backend consumer must not be only a conditional decoy")


def audit_scoped_surface_stub(source, signature, label):
    definition = definition_with_signature(production_preprocessor_view(source), signature)
    header = definition[:definition.find("{")]
    body_tokens = [token for token, _ in tokens(definition[definition.find("{"):])]
    require(re.search(r"const\s+GpuScopedNativeSurface\s*&(?:\s*[A-Za-z_]\w*)?", header)
            is not None,
            f"{label}: non-submitting backend must still accept the scoped native surface")
    require(not any(token in body_tokens for token in
                    ("GpuSurface", "GpuSyncReadScope", "GpuReadLease", "nativeHandle")),
            f"{label}: non-submitting backend must not recover another native surface")


def scoped_surface_escape_mutation_self_tests():
    safe = """
CVPixelBufferRef makePixelBufferWrapper(const GpuScopedNativeSurface& surface) {
    if (!surface.valid()) return nullptr;
    return retainApplePixelBufferWrapper(surface);
}
"""
    audit_scoped_surface_backend(
        safe, "CVPixelBufferRef makePixelBufferWrapper",
        "safe scoped-surface escape mutation", ("retainApplePixelBufferWrapper",))
    for injected, message in (
        ("    const auto& alias = surface; globalSlot = &alias;\n",
         "a scoped-surface reference alias must not escape to global storage"),
        ("    member.slot = &surface;\n",
         "a scoped-surface pointer must not escape to member storage"),
        ("    escapedSlots.push_back(&surface);\n",
         "a scoped-surface pointer must not escape through a container"),
        ("    auto wrapped = std::ref(surface); store(wrapped);\n",
         "a scoped surface must not escape through std::ref"),
        ("    auto wrapped = std::cref(surface); store(wrapped);\n",
         "a scoped surface must not escape through std::cref"),
        ("    std::reference_wrapper<const GpuScopedNativeSurface> wrapped(surface); "
         "store(wrapped);\n",
         "a scoped surface must not escape through std::reference_wrapper"),
        ("    auto deferred = [&surface] { consumeLater(); }; queue(deferred);\n",
         "a scoped-surface reference must not enter a deferred lambda capture"),
        ("    const auto& alias = surface; auto deferred = [&alias] { consume(alias); };\n",
         "an alias must not bypass deferred scoped-surface capture enforcement"),
    ):
        mutation = safe.replace("    if (!surface.valid())", injected +
                                "    if (!surface.valid())")
        try:
            audit_scoped_surface_backend(
                mutation, "CVPixelBufferRef makePixelBufferWrapper",
                "scoped-surface escape mutation", ("retainApplePixelBufferWrapper",))
        except AssertionError:
            continue
        raise AssertionError(message)

def native_submit_adapter_mutation_self_tests():
    safe_readback = """
void submitReadback() {
    GpuOpScope operation(fence, registry);
    auto adapter = [&](const GpuScopedNativeView<1>& view) noexcept {
        result = rhi->importAndReadback(view.get<0>(), target);
        return result.outcome;
    };
    (void) operation.submit(adapter, GpuSurfacePack<1>({surface}));
}
"""
    audit_native_submit_source(safe_readback, "playback/gpu/safe_readback.cpp")
    for injected, message in (
        ("        const auto* escaped = &view[0]; storeForLater(escaped);\n",
         "a submitted scoped-view slot pointer must not escape"),
        ("        const auto& alias = view[0]; globalSlot = &alias;\n",
         "a submitted scoped-view slot reference must not escape"),
        ("        escapedSlots.push_back(&view[0]);\n",
         "a submitted scoped-view slot must not enter a container"),
        ("        auto wrapped = std::ref(view[0]); storeForLater(wrapped);\n",
         "a submitted scoped-view slot must not escape through std::ref"),
        ("        auto wrapped = std::cref(view.get<0>()); storeForLater(wrapped);\n",
         "a submitted scoped-view slot must not escape through std::cref"),
        ("        const auto& alias = view[0]; "
         "auto deferred = [&alias] { consume(alias); }; queue(deferred);\n",
         "a submitted scoped-view alias must not enter a deferred capture"),
    ):
        mutation = safe_readback.replace(
            "        result = rhi->importAndReadback", injected +
            "        result = rhi->importAndReadback")
        try:
            audit_native_submit_source(mutation, "playback/gpu/unsafe_readback.cpp")
        except AssertionError:
            continue
        raise AssertionError(message)

    generic_escape = safe_readback.replace(
        "const GpuScopedNativeView<1>& view", "const auto& view").replace(
        "        result = rhi->importAndReadback",
        "        const auto* escaped = &view[0]; storeForLater(escaped);\n"
        "        result = rhi->importAndReadback")
    try:
        audit_native_submit_source(generic_escape, "playback/gpu/generic_escape.cpp")
    except AssertionError:
        pass
    else:
        raise AssertionError("a generic native-submit adapter must not bypass view escape audit")

    aliased_scope_escape = safe_readback.replace(
        "    (void) operation.submit(adapter,",
        "    auto& operationAlias = operation;\n"
        "    (void) operationAlias.submit(adapter,").replace(
        "        result = rhi->importAndReadback",
        "        const auto* escaped = &view[0]; storeForLater(escaped);\n"
        "        result = rhi->importAndReadback")
    try:
        audit_native_submit_source(aliased_scope_escape, "playback/gpu/aliased_scope.cpp")
    except AssertionError:
        pass
    else:
        raise AssertionError("a GpuOpScope alias must not bypass native adapter discovery")

    for mutation, message in (
        (safe_readback.replace(
            "    (void) operation.submit(adapter, GpuSurfacePack<1>({surface}));",
            "    auto member = &GpuOpScope::submit<decltype(adapter), 1>;\n"
            "    (operation.*member)(adapter, GpuSurfacePack<1>({surface}));"),
         "a native-submit member pointer must be rejected"),
        (safe_readback.replace(
            "    (void) operation.submit(adapter, GpuSurfacePack<1>({surface}));",
            "#define NATIVE_SUBMIT submit\n"
            "    (void) operation.NATIVE_SUBMIT(adapter, GpuSurfacePack<1>({surface}));"),
         "an object-like native-submit alias must be rejected"),
    ):
        try:
            audit_native_submit_source(mutation, "playback/gpu/native_indirection.cpp")
        except AssertionError:
            continue
        raise AssertionError(message)

    parenthesized_template = safe_readback.replace(
        "operation.submit(adapter,", "(((operation))).submit<decltype(adapter), 1>(adapter,")
    audit_native_submit_source(parenthesized_template,
                               "playback/gpu/parenthesized_template.cpp")
    dependent_template = safe_readback.replace(
        "operation.submit(adapter,", "operation.template submit<decltype(adapter), 1>(adapter,")
    audit_native_submit_source(dependent_template,
                               "playback/gpu/dependent_template.cpp")
    alias_typed_direct = safe_readback.replace(
        "void submitReadback() {", "using ScopeAlias = GpuOpScope;\n"
        "typedef ScopeAlias TransitiveScope;\nvoid submitReadback() {").replace(
        "GpuOpScope operation", "TransitiveScope operation")
    audit_native_submit_source(alias_typed_direct, "playback/gpu/alias_typed_direct.cpp")

    for declaration in (
        "using ScopeAlias = GpuOpScope;",
        "typedef GpuOpScope ScopeAlias;",
        "using FirstScope = GpuOpScope; using ScopeAlias = FirstScope;",
        "using ScopeAlias = ::GpuOpScope;",
        "namespace gpu { using FirstScope = ::GpuOpScope; } "
        "using ScopeAlias = gpu::FirstScope;",
    ):
        mutation = safe_readback.replace(
            "void submitReadback() {", declaration + "\nvoid submitReadback() {").replace(
            "    (void) operation.submit(adapter, GpuSurfacePack<1>({surface}));",
            "    auto member = &ScopeAlias::submit<decltype(adapter), 1>;\n"
            "    consume(member);")
        try:
            audit_native_submit_source(mutation, "playback/gpu/alias_member_pointer.cpp")
        except AssertionError:
            continue
        raise AssertionError("a GpuOpScope type alias must not hide a submit member pointer")

    unrelated_qualified_type = """
namespace ordinary { struct GpuOpScope {}; }
using ScopeAlias = ordinary::GpuOpScope;
void decoy() { auto member = &ScopeAlias::submit<Adapter, 1>; consume(member); }
"""
    audit_gpu_scope_method_directness(
        unrelated_qualified_type, "submit", "project/unrelated_scope_alias.cpp")

    for source in (
        "void bad() { auto member = &::GpuOpScope::submit<Adapter, 1>; }",
        "namespace gpu { using ScopeAlias = ::GpuOpScope; } "
        "void bad() { auto member = &gpu::ScopeAlias::submit<Adapter, 1>; }",
        "#define SCOPE_OWNER ::GpuOpScope\n"
        "void bad() { auto member = &SCOPE_OWNER::submit<Adapter, 1>; }",
        "#define SCOPE_OWNER() ::GpuOpScope\n"
        "void bad() { auto member = &SCOPE_OWNER()::submit<Adapter, 1>; }",
        "#define SUBMIT_MEMBER submit\n"
        "void bad() { auto member = &::GpuOpScope::SUBMIT_MEMBER<Adapter, 1>; }",
        "#define SUBMIT_MEMBER() submit\n"
        "void bad() { auto member = &::GpuOpScope::SUBMIT_MEMBER()<Adapter, 1>; }",
    ):
        try:
            audit_gpu_scope_method_directness(source, "submit", "qualified submit pointer")
        except AssertionError:
            continue
        raise AssertionError("a qualified or macro submit member pointer was accepted")

    namespace_collision = """
struct OtherScope {};
namespace gpu { using ScopeAlias = ::GpuOpScope; }
namespace ordinary { using ScopeAlias = ::OtherScope; }
void decoy() { auto member = &ordinary::ScopeAlias::submit<Adapter, 1>; consume(member); }
"""
    audit_gpu_scope_method_directness(
        namespace_collision, "submit", "project/namespace_collision.cpp")

    native_decoy = """
#define NATIVE_SUBMIT submit
struct Queue { void submit(int); };
void decoy(GpuOpScope& operation, Queue& queue) {
    consume(operation, queue.NATIVE_SUBMIT(1));
}
"""
    audit_native_submit_source(native_decoy, "project/native_submit_decoy.cpp")
    literal_native_decoy = """
struct Queue { void submit(int); };
void decoy(GpuOpScope& operation, Queue& queue) {
    consume(operation, queue.submit(1));
}
"""
    audit_native_submit_source(literal_native_decoy,
                               "project/literal_native_submit_decoy.cpp")

    for mutation in (
        safe_readback.replace(
            "    (void) operation.submit(adapter,",
            "    auto& hidden = (operation);\n    (void) hidden.submit(adapter,").replace(
            "        result = rhi->importAndReadback",
            "        const auto* escaped = &view[0]; storeForLater(escaped);\n"
            "        result = rhi->importAndReadback"),
        safe_readback.replace(
            "    (void) operation.submit(adapter,",
            "    auto&& hidden = std::move(operation);\n"
            "    (void) hidden.submit(adapter,").replace(
            "        result = rhi->importAndReadback",
            "        const auto* escaped = &view[0]; storeForLater(escaped);\n"
            "        result = rhi->importAndReadback"),
        safe_readback.replace(
            "    (void) operation.submit(adapter,",
            "    auto&& hidden = std::move((operation));\n"
            "    (void) hidden.submit(adapter,").replace(
            "        result = rhi->importAndReadback",
            "        const auto* escaped = &view[0]; storeForLater(escaped);\n"
            "        result = rhi->importAndReadback"),
        safe_readback.replace(
            "    (void) operation.submit(adapter,",
            "    auto&& hidden = std::forward<std::type_identity_t<GpuOpScope>&>"
            "(operation);\n    (void) hidden.submit(adapter,").replace(
            "        result = rhi->importAndReadback",
            "        const auto* escaped = &view[0]; storeForLater(escaped);\n"
            "        result = rhi->importAndReadback"),
        safe_readback.replace(
            "void submitReadback() {",
            "GpuOpScope& identity(GpuOpScope& scope) { return scope; }\n"
            "void submitReadback() {").replace(
            "    (void) operation.submit(adapter,",
            "    auto&& hidden = identity(operation);\n"
            "    (void) hidden.submit(adapter,").replace(
            "        result = rhi->importAndReadback",
            "        const auto* escaped = &view[0]; storeForLater(escaped);\n"
            "        result = rhi->importAndReadback"),
        safe_readback.replace(
            "    (void) operation.submit(adapter,",
            "    auto* hidden = &operation;\n    (void) hidden->submit(adapter,").replace(
            "        result = rhi->importAndReadback",
            "        const auto* escaped = &view[0]; storeForLater(escaped);\n"
            "        result = rhi->importAndReadback"),
        "namespace gpu { using ScopeAlias = ::GpuOpScope; "
        "void bad() { ScopeAlias operation(fence, registry); "
        "auto adapter = [&](const auto& view) noexcept { "
        "const auto* escaped = &view[0]; storeForLater(escaped); "
        "return GpuSubmitOutcome::Submitted; }; operation.submit(adapter, pack); } }",
    ):
        try:
            audit_native_submit_source(mutation, "playback/gpu/hidden_scope.cpp")
        except AssertionError:
            continue
        raise AssertionError(
            "a parenthesized, expression, or namespace-local scope alias hid an escape")

    ordinary_namespace = """
namespace ordinary {
struct GpuOpScope {};
void safe() {
    GpuOpScope operation;
    auto adapter = [&](const auto& view) noexcept {
        const auto* ordinaryUse = &view[0]; consumeSynchronously(ordinaryUse);
        return GpuSubmitOutcome::Submitted;
    };
    operation.submit(adapter, pack);
}
}
"""
    audit_native_submit_source(ordinary_namespace, "project/ordinary_namespace.cpp")

    for decoy in (
        "void safe() { GpuOpScope operation(fence, registry); OtherScope decoy; "
        "auto&& hidden = other(decoy); hidden.submit(adapter, pack); }",
        "void safe() { GpuOpScope operation(fence, registry); OtherScope decoy; "
        "auto&& hidden = std::move(decoy); hidden.submit(adapter, pack); }",
        "void safe() { GpuOpScope operation(fence, registry); "
        "bool value = other(operation); consume(value); }",
    ):
        require("hidden" not in gpu_op_scope_names(decoy),
                "non-identity or other-operand reference initializer became protected")
        audit_gpu_scope_method_directness(decoy, "submit", "project/reference_decoy.cpp")

    carrier = (
        "void bad() { GpuOpScope operation(fence, registry); "
        "auto holder = std::ref(operation); consume(holder.get()); }")
    try:
        audit_native_submit_source(carrier, "project/reference_carrier.cpp")
    except AssertionError:
        pass
    else:
        raise AssertionError("std::ref(operation) escaped the canonical binding grammar")

    safe_compositor = """
FrameHandle compose() {
    GpuOpScope operation(fence, registry);
    auto adapter = [&](const auto& nativeView) noexcept {
        const bool invoked = rhi->invokeOnRenderThread([&](QRhi* rhi) {
            auto nativeAt = [&](size_t slot) -> const GpuScopedNativeSurface* {
                return slot < nativeView.size() ? &nativeView[slot] : nullptr;
            };
            renderResult = renderGridWithRhi(rhi, sources, count, width, height,
                                             color, quality, nativeAt, outputSlot);
        });
        if (!invoked || !renderResult.submissionAttempted)
            return GpuSubmitOutcome::NotSubmitted;
        return renderResult.rendered ? GpuSubmitOutcome::Submitted
                                     : GpuSubmitOutcome::SubmittedWithError;
    };
    return submitCompactedOwners<1>(operation, adapter, owners, ownerCount);
}
"""
    audit_native_submit_source(safe_compositor, "playback/gpu/gpucompositor.cpp")
    compositor_escape = safe_compositor.replace(
        "            renderResult = renderGridWithRhi",
        "            escapedSlots.push_back(&nativeView[slot]);\n"
        "            renderResult = renderGridWithRhi")
    try:
        audit_native_submit_source(compositor_escape, "playback/gpu/gpucompositor.cpp")
    except AssertionError:
        pass
    else:
        raise AssertionError("the compositor exception must not permit a second slot escape")

    for injected, message in (
        ("            escapedAccessors.push_back(nativeAt);\n",
         "the compositor accessor must not enter storage"),
        ("            auto deferred = [nativeAt] { return nativeAt; }; queue(deferred);\n",
         "the compositor accessor must not enter a deferred capture"),
        ("            auto returned = returnAccessor(nativeAt); consume(returned);\n",
         "the compositor accessor must not be returned or passed elsewhere"),
    ):
        mutation = safe_compositor.replace(
            "            renderResult = renderGridWithRhi", injected +
            "            renderResult = renderGridWithRhi")
        try:
            audit_native_submit_source(mutation, "playback/gpu/gpucompositor.cpp")
        except AssertionError:
            continue
        raise AssertionError(message)

    for definition, invocation, message in (
        ("#define STORE_ACCESSOR escapedAccessors.push_back(nativeAt)\n",
         "            STORE_ACCESSOR;\n",
         "an object macro must not store the compositor accessor"),
        ("#define STORE_SLOT() escapedSlots.push_back(&nativeView[slot])\n",
         "            STORE_SLOT();\n",
         "a function macro must not store a scoped native-view slot"),
        ("#define DEFER_ACCESSOR() auto later = [nativeAt] { return nativeAt; }; queue(later)\n",
         "            DEFER_ACCESSOR();\n",
         "a macro must not copy/return/capture the compositor accessor"),
    ):
        mutation = definition + safe_compositor.replace(
            "            renderResult = renderGridWithRhi", invocation +
            "            renderResult = renderGridWithRhi")
        try:
            audit_native_submit_source(mutation, "playback/gpu/gpucompositor.cpp")
        except AssertionError:
            continue
        raise AssertionError(message)

    safe_macro = "#define RECORD_RENDER() recordRenderMetric()\n" + safe_compositor.replace(
        "            renderResult = renderGridWithRhi",
        "            RECORD_RENDER();\n            renderResult = renderGridWithRhi")
    audit_native_submit_source(safe_macro, "playback/gpu/gpucompositor.cpp")


def first_party_source_discovery_mutation_self_tests():
    covered = (
        "project/source.cpp", "telemetry/source.cc", "websocket/source.cxx",
        "midi/source.mm", "streamdeck/source.h", "builder/source.cpp",
        "build_support/source.cpp", "root_source.cpp",
    )
    require(all(is_first_party_source_path(path) for path in covered),
            "whole-tree GPU audit must cover every first-party source root and root source")
    excluded = (
        "tests/source.cpp", "build/source.cpp", "build-review/source.cpp",
        "docs/source.cpp", "handoff-notes/source.cpp", "third_party/source.cpp",
        "vendor/source.cpp", "dependencies/source.cpp", "windows_build/source.cpp",
        "windows_build/dist/dependency.cpp", ".claude/worktrees/source.cpp",
    )
    require(not any(is_first_party_source_path(path) for path in excluded),
            "whole-tree GPU audit must exclude tests, generated trees, docs, and dependencies")
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        for relative in covered + excluded:
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("// fixture\n", encoding="utf-8")
        discovered = {
            path.relative_to(root).as_posix() for path in first_party_source_paths(root)
        }
        require(discovered == set(covered),
                "whole-tree GPU audit discovery did not match the first-party fixture")


def audit_compositor_backend_primitive(compositor, label):
    function = function_block(compositor, "RenderGridResult renderGridWithRhi")
    function_tokens = [token for token, _ in tokens(function)]
    require(not any(token in function_tokens for token in
                    ("GpuSurface", "GpuSyncReadScope", "GpuReadLease", "nativeHandle", "owners")),
            f"{label}: render primitive must not recover an independent surface owner")
    require(len(re.findall(r"\bnativeAt\s*\(", function)) == 2,
            f"{label}: native output and source must each resolve through the scoped accessor")
    require(re.search(r"outputSurface\s*=\s*nativeAt\s*\(\s*outputSlot\s*\)", function) and
            re.search(r"importRgbaRenderTarget\s*\(\s*rhi\s*,\s*\*\s*outputSurface\s*\)",
                      function),
            f"{label}: output import must consume the exact accessor slot")
    require(re.search(r"nativeSource\s*=\s*nativeAt\s*\(\s*source\s*\.\s*nativeSlot\s*\)",
                      function) and
            re.search(r"importNv12Source\s*\(\s*rhi\s*,\s*\*\s*nativeSource\s*\)", function),
            f"{label}: source import must consume the exact accessor slot")


def audit_exact_retained_owner_production(repo_root, frame_data, compositor, apple_surface):
    audit_exact_readback_owner_binding(frame_data, "GpuFrameData exact readback owner")
    audit_exact_compositor_owner_binding(
        function_block(compositor, "FrameHandle GpuCompositor::composeGridForGeneration"),
        "GPU compositor native output exact owners")
    audit_exact_compositor_owner_binding(
        function_block(compositor, "CpuPlanes GpuCompositor::composeGridToCpuForGeneration"),
        "GPU compositor readback exact owners")
    audit_compositor_backend_primitive(compositor, "GPU compositor render backend")

    rhi_header = (repo_root / "playback/gpu/gpurhicontext.h").read_text(encoding="utf-8")
    require(re.search(r"importAndReadback\s*\(\s*const\s+GpuScopedNativeSurface\s*&\s*surface",
                      strip_comments_and_literals(rhi_header)) is not None,
            "GpuRhiContext raw readback boundary must require a scoped native surface")
    apple_rhi = (repo_root / "playback/gpu/gpurhicontext_apple.mm").read_text(encoding="utf-8")
    audit_scoped_surface_backend(
        apple_rhi, "GpuReadbackResult GpuRhiContext::importAndReadback",
        "Apple RHI readback backend", ("retainApplePixelBufferWrapper",),
        allow_direct_native_handle=True)
    for path, label in ((repo_root / "playback/gpu/gpurhicontext_win.cpp", "Windows RHI readback"),
                        (repo_root / "playback/gpu/gpurhicontext_stub.cpp", "stub RHI readback")):
        audit_scoped_surface_backend(
            path.read_text(encoding="utf-8"),
            "GpuReadbackResult GpuRhiContext::importAndReadback", label,
            allow_direct_native_handle=True)

    audit_scoped_surface_backend(
        apple_surface, "CVPixelBufferRef retainApplePixelBufferWrapper(const GpuScopedNativeSurface&",
        "Apple scoped pixel-buffer wrapper", allow_direct_native_handle=True)
    compositor_apple = (repo_root / "playback/gpu/gpucompositor_apple.mm").read_text(
        encoding="utf-8")
    audit_scoped_surface_backend(
        compositor_apple, "CVPixelBufferRef makePixelBufferWrapper",
        "Apple compositor pixel-buffer wrapper", ("retainApplePixelBufferWrapper",))
    audit_scoped_surface_backend(
        compositor_apple, "std::unique_ptr<ImportedNv12Source> importNv12Source",
        "Apple compositor NV12 import", ("makePixelBufferWrapper",))
    audit_scoped_surface_backend(
        compositor_apple, "importRgbaRenderTarget",
        "Apple compositor RGBA output import", ("makePixelBufferWrapper",))
    audit_scoped_surface_stub(compositor,
                              "std::unique_ptr<ImportedNv12Source> importNv12Source",
                              "stub compositor NV12 import")
    audit_scoped_surface_stub(compositor,
                              "std::unique_ptr<ImportedRgbaRenderTarget>\nimportRgbaRenderTarget",
                              "stub compositor RGBA output import")


def class_access_at(class_body, position):
    access = "private"
    for match in re.finditer(r"\b(public|private|protected)\s*:", class_body[:position]):
        access = match.group(1)
    return access


def audit_fixed_surface_watermark(surface_header, label):
    body = function_block(surface_header, "class GpuSurface")
    retain_signature = re.search(r"\bvoid\s+retainUntilFenceRetired\s*\(", body)
    pending_signature = re.search(r"\buint64_t\s+pendingFenceValue\s*\(", body)
    require(retain_signature is not None and pending_signature is not None,
            f"{label}: fixed retain and pending operations are required")
    require(class_access_at(body, retain_signature.start()) == "public" and
            class_access_at(body, pending_signature.start()) == "public",
            f"{label}: retain and pending operations must remain public")
    for signature in (retain_signature, pending_signature):
        statement_start = max(body.rfind(";", 0, signature.start()),
                              body.rfind("{", 0, signature.start())) + 1
        declaration_end = body.find("{", signature.end())
        declaration = body[statement_start:declaration_end]
        require("virtual" not in declaration and "final" not in declaration and
                "noexcept" in declaration,
                f"{label}: retain and pending operations must be fixed nonvirtual noexcept "
                "operations")
    retain = function_block(body, "void retainUntilFenceRetired")
    pending = function_block(body, "uint64_t pendingFenceValue")
    members = list(re.finditer(
        r"(?:mutable\s+)?std\s*::\s*atomic\s*<\s*uint64_t\s*>\s+([A-Za-z_]\w*)\b",
        body,
    ))
    require(len(members) == 1 and class_access_at(body, members[0].start()) == "private",
            f"{label}: exact watermark must be one private atomic<uint64_t> member")
    watermark = members[0].group(1)
    retain_values = [token for token, _ in tokens(retain)]
    pending_values = [token for token, _ in tokens(pending)]

    def has_sequence(values, expected):
        return any(values[index:index + len(expected)] == expected
                   for index in range(len(values) - len(expected) + 1))

    require(has_sequence(
        retain_values,
        ["uint64_t", "previous", "=", watermark, ".", "load", "(", "std", "::",
         "memory_order_relaxed", ")"],
    ), f"{label}: monotonic retain must seed its CAS from the exact base watermark")
    require(has_sequence(
        retain_values,
        ["fenceValue", ">", "previous", "&&", "!", watermark, ".",
         "compare_exchange_weak", "(", "previous", ",", "fenceValue", ",", "std", "::",
         "memory_order_relaxed", ",", "std", "::", "memory_order_relaxed", ")"],
    ), f"{label}: retain must be a value-only relaxed monotonic-max CAS on the exact watermark")
    require(retain_values.count("compare_exchange_weak") == 1 and
            not any(token in retain_values for token in
                    ("store", "exchange", "fetch_add", "fetch_or")),
            f"{label}: retain must expose only the reviewed monotonic-max write")
    require(has_sequence(
        pending_values,
        ["return", watermark, ".", "load", "(", "std", "::",
         "memory_order_relaxed", ")", ";"],
    ), f"{label}: pending query must relaxed-load the exact base watermark")


def audit_publish_then_stamp(submit, label):
    items = tokens(submit)
    values = [token for token, _ in items]
    parens = token_pairs(items, "(", ")")
    braces = token_pairs(items, "{", "}")
    publishes = [index for index, token in enumerate(values) if token == "publishPrepared"]
    stamps = [index for index, token in enumerate(values) if token == "retainUntilFenceRetired"]
    require(len(publishes) == 1 and len(stamps) == 1,
            f"{label}: submit path must publish once and contain one all-owner stamp loop")
    publish = publishes[0]
    stamp = stamps[0]
    require(publish < stamp,
            f"{label}: pending-fence stamp must follow successful retirement publication")
    def has_sequence(expected, start=0, end=None):
        if end is None:
            end = len(values)
        return any(values[index:index + len(expected)] == expected
                   for index in range(start, end - len(expected) + 1))

    require(has_sequence(
        ["retirementOwners", "=", "surfaces", ".", "m_surfaces", ".", "data", "(", ")"],
        end=publish,
    ), f"{label}: retirement owner array must begin with the exact submitted owner pack")
    require(has_sequence(
        ["[", "i", "]", "=", "surfaces", ".", "m_surfaces", "[", "uniqueIndices", "[",
         "i", "]", "]"],
        end=publish,
    ) and has_sequence(
        ["retirementOwners", "=", "coalescedOwners", "->", "data", "(", ")"],
        end=publish,
    ), f"{label}: duplicate owners must coalesce from the exact unique submitted owners")
    require(has_sequence(
        ["prepareRetirement", "(", "retirementOwners", ",", "qsizetype", "(",
         "uniqueCount", ")", ",", "m_fence", ")"],
        end=publish,
    ), f"{label}: retirement must retain the same deduplicated owner array that is stamped")
    condition_open = next(
        (opening for opening, closing in parens.items()
         if opening < closing and opening < publish < closing and
         opening > 0 and values[opening - 1] == "if"),
        None,
    )
    require(condition_open is not None,
            f"{label}: publishPrepared must be the success condition of an if statement")
    condition_close = parens[condition_open]
    require("exact" in values[condition_open + 1:publish] and
            values[publish - 2:publish + 2] == ["m_registry", ".", "publishPrepared", "("],
            f"{label}: stamp authority requires exact successful registry publication")
    success_open = next((index for index in range(condition_close + 1, len(values))
                         if values[index] == "{"), None)
    require(success_open is not None and success_open < stamp < braces[success_open],
            f"{label}: stamp must exist only in publishPrepared's success body")
    require(not is_obviously_unreachable(items, stamp, braces),
            f"{label}: post-publication stamp loop must be reachable")
    stamp_shape = ["retirementOwners", "[", "i", "]", "->",
                   "retainUntilFenceRetired", "(", "ticketValue", ")"]
    require(values[stamp - 5:stamp + 4] == stamp_shape,
            f"{label}: stamp must target every exact unique retained owner and ticket value")
    loop_open = next(
        (opening for opening, closing in parens.items()
         if opening < closing and opening < stamp < braces.get(
             next((candidate for candidate in range(closing + 1, len(values))
                   if values[candidate] == "{"), len(values)), -1)
         and opening > 0 and values[opening - 1] == "for"),
        None,
    )
    require(loop_open is not None and
            ["i", "<", "uniqueCount"] in [values[index:index + 3]
                                             for index in range(loop_open, parens[loop_open] - 2)],
            f"{label}: stamp loop must cover every unique retained compatibility")
    published = next((index for index in range(stamp + 1, braces[success_open])
                      if values[index:index + 7] ==
                      ["result", ".", "retirement", "=", "GpuRetirementDisposition", "::",
                       "Published"]), None)
    require(published is not None,
            f"{label}: result may become Published only after all watermarks are stamped")


def audit_two_field_surface_compatibility(submission, label):
    body = function_block(submission, "struct GpuSurfaceCompatibility")
    fields = re.findall(
        r"\b(uintptr_t|uint64_t)\s+([A-Za-z_]\w*)\s*=\s*0\s*;",
        body,
    )
    require(fields == [("uintptr_t", "deviceDomainId"), ("uint64_t", "authorityEpoch")],
            f"{label}: compatibility evidence must remain exactly the two 64-bit authority "
            "fields")
    require("GpuPendingFenceStamp" not in [token for token, _ in tokens(body)] and
            body.count(";") == 2,
            f"{label}: compatibility POD must not regain a fence-stamp carrier or extra field")


def audit_retained_only_submission_api(gpu_op_scope, label):
    retained = function_block(gpu_op_scope, "GpuSubmissionResult submitRetained")
    retained_header = retained[:retained.find("{")]
    retained_values = [token for token, _ in tokens(retained)]
    require(not any(token in retained_header for token in
                    ("GpuScopedNativeView", "GpuScopedNativeSurface", "GpuReadLease",
                     "GpuSyncReadScope")),
            f"{label}: retained-only callback API must expose no scoped/read capability")
    require(re.search(
        r"is_nothrow_invocable_r_v\s*<\s*GpuSubmitOutcome\s*,\s*BackendAdapter\s*&\s*>",
        retained,
    ) is not None,
            f"{label}: retained-only adapter must be a noexcept zero-argument callback")
    require(any(retained_values[index:index + 6] ==
                ["return", "submitImpl", "<", "false", ">", "("]
                for index in range(len(retained_values) - 5)),
            f"{label}: retained-only public API must select the non-native implementation")

    implementation = function_block(gpu_op_scope, "GpuSubmissionResult submitImpl")
    cleaned = strip_comments_and_literals(implementation)
    require(re.search(
        r"if\s+constexpr\s*\(\s*ExposeNative\s*\)\s*"
        r"outcome\s*=\s*std\s*::\s*invoke\s*\(\s*adapter\s*,\s*\*\s*view\s*\)\s*;\s*"
        r"else\s*outcome\s*=\s*std\s*::\s*invoke\s*\(\s*adapter\s*\)\s*;",
        cleaned,
        re.DOTALL,
    ) is not None,
            f"{label}: only native submissions may pass a scoped view to the adapter")
    implementation_items = tokens(cleaned)
    implementation_values = [token for token, _ in implementation_items]
    implementation_parens = token_pairs(implementation_items, "(", ")")
    implementation_braces = token_pairs(implementation_items, "{", "}")
    guarded_branches = []
    for index in range(len(implementation_values) - 3):
        if implementation_values[index:index + 4] != ["if", "constexpr", "(", "ExposeNative"]:
            continue
        condition_open = index + 2
        condition_close = implementation_parens.get(condition_open)
        if condition_close is None or condition_close + 1 >= len(implementation_values):
            continue
        true_open = condition_close + 1
        if implementation_values[true_open] != "{" or true_open not in implementation_braces:
            continue
        true_close = implementation_braces[true_open]
        if implementation_values[true_close + 1:true_close + 3] != ["else", "{"]:
            continue
        false_open = true_close + 2
        false_close = implementation_braces.get(false_open)
        if false_close is None:
            continue
        true_values = implementation_values[true_open + 1:true_close]
        false_values = implementation_values[false_open + 1:false_close]
        if "withScopedNativeView" in true_values:
            guarded_branches.append((true_values, false_values))
    require(len(guarded_branches) == 1 and
            "submitScoped" in guarded_branches[0][1] and
            "nullptr" in guarded_branches[0][1] and
            not any(token in guarded_branches[0][1] for token in
                    ("GpuScopedNativeView", "GpuScopedNativeSurface", "withScopedNativeView",
                     "view", "nativeHandle", "GpuSyncReadScope", "GpuReadLease")),
            f"{label}: retained-only execution must bypass scoped-view construction entirely")


RETAINED_ONLY_FORBIDDEN_PATHS = {
        "playback/gpu/gpuframedata.cpp",
        "playback/gpu/gpucompositor.cpp",
        "playback/gpu/gpucompositor_apple.mm",
        "playback/gpu/gpurhicontext_apple.mm",
        "playback/gpu/gpurhicontext_stub.cpp",
        "playback/gpu/gpurhicontext_win.cpp",
}


def gpu_scope_member_call_sites(source, method):
    """Return direct calls to a method on a known GpuOpScope binding or alias."""
    items = tokens(source)
    scope_names = gpu_op_scope_names(source)
    sites = []
    for index, (token, position) in enumerate(items):
        if token != method:
            continue
        opening = call_open_after_template_id(items, index)
        if opening is None:
            continue
        operator = index - 1
        if operator >= 0 and items[operator][0] == "template":
            operator -= 1
        if operator < 1 or items[operator][0] not in (".", "->"):
            continue
        receiver = immediate_receiver_name(items, operator)
        if receiver not in scope_names:
            continue
        sites.append((position, items[opening][1]))
    return sites


def retained_only_call_sites(source):
    """Return direct GpuOpScope submitRetained calls, including template syntax."""
    return [(position, opening_position + 1)
            for position, opening_position in
            gpu_scope_member_call_sites(source, "submitRetained")]


def call_arguments(items, opening):
    """Split one tokenized call into top-level argument token lists."""
    arguments = []
    current = []
    paren = bracket = brace = angle = 0
    for index in range(opening + 1, len(items)):
        token = items[index][0]
        if token == ")" and paren == bracket == brace == angle == 0:
            arguments.append(current)
            return arguments
        if token == "(" :
            paren += 1
        elif token == ")":
            paren -= 1
        elif token == "[":
            bracket += 1
        elif token == "]":
            bracket -= 1
        elif token == "{":
            brace += 1
        elif token == "}":
            brace -= 1
        elif token == "<":
            angle += 1
        elif token == ">" and angle:
            angle -= 1
        elif token == "," and paren == bracket == brace == angle == 0:
            arguments.append(current)
            current = []
            continue
        current.append(items[index])
    raise AssertionError("unterminated audited call")


def call_open_after_template_id(items, identifier_index):
    cursor = identifier_index + 1
    if cursor < len(items) and items[cursor][0] == "<":
        depth = 0
        while cursor < len(items):
            token = items[cursor][0]
            if token == "<":
                depth += 1
            elif token == ">":
                depth -= 1
                require(depth >= 0, "unbalanced audited template arguments")
                if depth == 0:
                    cursor += 1
                    break
            cursor += 1
        require(depth == 0, "unterminated audited template arguments")
    return cursor if cursor < len(items) and items[cursor][0] == "(" else None


def single_identifier(argument):
    values = [token for token, _ in argument]
    while len(values) >= 2 and values[0] == "(" and values[-1] == ")":
        values = values[1:-1]
    return values[0] if len(values) == 1 and re.fullmatch(r"[A-Za-z_]\w*", values[0]) else None


def gpu_op_scope_type_names(source):
    if "GpuOpScope" not in source:
        return {"GpuOpScope"}
    qualified_type = r"(?:::)?[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*"
    namespace_blocks = []
    for match in re.finditer(
            r"\bnamespace\s+([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\s*\{", source):
        opening = source.find("{", match.start(), match.end())
        namespace_blocks.append((opening, matching_character(source, opening, "{", "}"),
                                 tuple(match.group(1).split("::"))))

    def namespace_at(position):
        result = ()
        for opening, closing, components in sorted(namespace_blocks):
            if opening < position < closing:
                result += components
        return result

    def candidates(target, namespace):
        global_name = target.startswith("::")
        parts = tuple(target[2:].split("::") if global_name else target.split("::"))
        if global_name:
            return (parts,)
        return tuple(namespace[:depth] + parts for depth in range(len(namespace), -1, -1))

    aliases = []
    for match in re.finditer(
            rf"\busing\s+([A-Za-z_]\w*)\s*=\s*({qualified_type})\s*;", source):
        aliases.append((namespace_at(match.start()), match.group(1), match.group(2)))
    for match in re.finditer(
            rf"\btypedef\s+({qualified_type})\s+([A-Za-z_]\w*)\s*;", source):
        aliases.append((namespace_at(match.start()), match.group(2), match.group(1)))

    declarations = {
        namespace_at(match.start()) + (match.group(1),)
        for match in re.finditer(
            r"\b(?:class|struct|union|enum)\s+([A-Za-z_]\w*)\b", source)
    }
    symbols = {("GpuOpScope",)}
    changed = True
    while changed:
        changed = False
        for namespace, alias, target in aliases:
            symbol = namespace + (alias,)
            resolution = next(
                (candidate in symbols for candidate in candidates(target, namespace)
                 if candidate in symbols or candidate in declarations),
                False)
            if symbol not in symbols and resolution:
                symbols.add(symbol)
                changed = True
    return {"::".join(symbol) for symbol in symbols}


def gpu_op_scope_owner_is_protected(source, owner, position):
    symbols = {tuple(name.split("::")) for name in gpu_op_scope_type_names(source)}
    global_name = owner.startswith("::")
    parts = tuple(owner[2:].split("::") if global_name else owner.split("::"))
    if global_name:
        return parts in symbols
    namespace_blocks = []
    for match in re.finditer(
            r"\bnamespace\s+([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\s*\{", source):
        opening = source.find("{", match.start(), match.end())
        namespace_blocks.append((opening, matching_character(source, opening, "{", "}"),
                                 tuple(match.group(1).split("::"))))

    def namespace_at(location):
        result = ()
        for opening, closing, components in sorted(namespace_blocks):
            if opening < location < closing:
                result += components
        return result

    declarations = {
        namespace_at(match.start()) + (match.group(1),)
        for match in re.finditer(
            r"\b(?:class|struct|union|enum)\s+([A-Za-z_]\w*)\b", source)
    }
    namespace = namespace_at(position)
    for depth in range(len(namespace), -1, -1):
        candidate = namespace[:depth] + parts
        if candidate in symbols:
            return True
        if candidate in declarations:
            return False
    return False


def gpu_op_scope_owner_macros(source):
    protected = set()
    definitions = list(re.finditer(
        r"(?m)^\s*#\s*define\s+([A-Za-z_]\w*)(\s*\(\s*\))?\s+([^\n]+)", source))
    changed = True
    while changed:
        changed = False
        for definition in definitions:
            name, function, replacement = definition.groups()
            target = replacement.strip()
            if (gpu_op_scope_owner_is_protected(source, target, definition.start())
                    or target in protected):
                value = (name, function is not None)
                if value not in protected:
                    protected.add(value)
                    changed = True
    return protected


def member_pointer_has_protected_owner(source, items, scope_operator):
    cursor = scope_operator - 1
    macros = gpu_op_scope_owner_macros(source)
    if cursor >= 2 and items[cursor][0] == ")":
        if (items[cursor - 1][0] == "(" and
                (items[cursor - 2][0], True) in macros):
            return cursor >= 3 and items[cursor - 3][0] == "&"
        return False
    if cursor < 0 or not re.fullmatch(r"[A-Za-z_]\w*", items[cursor][0]):
        return False
    end = cursor
    cursor -= 1
    while (cursor >= 1 and items[cursor][0] == "::" and
           re.fullmatch(r"[A-Za-z_]\w*", items[cursor - 1][0])):
        cursor -= 2
    leading_global = cursor >= 0 and items[cursor][0] == "::"
    if leading_global:
        cursor -= 1
    if cursor < 0 or items[cursor][0] != "&":
        return False
    owner = "".join(value for value, _ in items[cursor + 1:end + 1])
    if (owner, False) in macros:
        return True
    return gpu_op_scope_owner_is_protected(source, owner, items[scope_operator][1])


def gpu_op_scope_reference_alias_pairs(source):
    """Parse the finite identity-initializer grammar for protected references."""
    items = tokens(source)
    values = [value for value, _position in items]

    def matching_paren(items, opening):
        depth = 0
        for index in range(opening, len(items)):
            depth += (items[index] == "(") - (items[index] == ")")
            if depth == 0:
                return index
        return None

    def unwrap_parentheses(items):
        result = list(items)
        while result and result[0] == "(":
            closing = matching_paren(result, 0)
            if closing != len(result) - 1:
                break
            result = result[1:-1]
        return result

    def identity_target(initializer):
        expression = unwrap_parentheses(initializer)
        if expression and expression[0] == "&":
            expression = unwrap_parentheses(expression[1:])
        if len(expression) == 1 and re.fullmatch(r"[A-Za-z_]\w*", expression[0]):
            return expression[0]
        if len(expression) < 6 or expression[:2] != ["std", "::"]:
            return None
        function = expression[2]
        cursor = 3
        if function == "forward":
            if cursor >= len(expression) or expression[cursor] != "<":
                return None
            depth = 0
            while cursor < len(expression):
                value = expression[cursor]
                depth += (value == "<") - (value == ">")
                cursor += 1
                if depth == 0:
                    break
            if depth != 0:
                return None
        elif function != "move":
            return None
        if cursor >= len(expression) or expression[cursor] != "(":
            return None
        closing = matching_paren(expression, cursor)
        if closing != len(expression) - 1:
            return None
        argument = unwrap_parentheses(expression[cursor + 1:closing])
        return (argument[0] if len(argument) == 1
                and re.fullmatch(r"[A-Za-z_]\w*", argument[0]) else None)

    def allowed_value_initializer(initializer):
        expression = unwrap_parentheses(initializer)
        if not expression:
            return None
        if expression[0] == "submitCompactedOwners":
            receiver = "*submitCompactedOwners*"
            cursor = 1
        elif (len(expression) >= 4
              and re.fullmatch(r"[A-Za-z_]\w*", expression[0])
              and expression[1] in {".", "->"}
              and expression[2] in {"submit", "submitRetained"}):
            receiver = expression[0]
            cursor = 3
        else:
            return None
        if cursor < len(expression) and expression[cursor] == "<":
            depth = 0
            while cursor < len(expression):
                depth += (expression[cursor] == "<") - (expression[cursor] == ">")
                cursor += 1
                if depth == 0:
                    break
        if (cursor >= len(expression) or expression[cursor] != "("
                or matching_paren(expression, cursor) != len(expression) - 1):
            return None
        if receiver == "*submitCompactedOwners*":
            arguments = []
            current = []
            depth = 0
            for token in expression[cursor + 1:-1]:
                depth += (token in {"(", "[", "{"}) - (token in {")", "]", "}"})
                if token == "," and depth == 0:
                    arguments.append(current)
                    current = []
                else:
                    current.append(token)
            arguments.append(current)
            exact = [["operation"], ["adapter"]]
            owners = [["retirementOwners"], ["retirementOwnerCount"]]
            compact = [["owners"], ["ownerCount"]]
            if len(arguments) != 4 or arguments[:2] != exact \
                    or arguments[2:] not in (owners, compact):
                return None
        return receiver

    declarations = []
    for index, value in enumerate(values):
        if value != "auto":
            continue
        cursor = index + 1
        if cursor < len(values) and values[cursor] == "const":
            cursor += 1
        reference_kind = None
        if cursor < len(values) and values[cursor] in {"&", "&&", "*"}:
            reference_kind = values[cursor]
            cursor += 1
        if cursor >= len(values) or not re.fullmatch(r"[A-Za-z_]\w*", values[cursor]):
            continue
        alias = values[cursor]
        cursor += 1
        if cursor >= len(values) or values[cursor] != "=":
            continue
        end = cursor + 1
        paren = bracket = brace = 0
        while end < len(values):
            token = values[end]
            if token == ";" and paren == bracket == brace == 0:
                break
            paren += (token == "(") - (token == ")")
            bracket += (token == "[") - (token == "]")
            brace += (token == "{") - (token == "}")
            end += 1
        if end >= len(values):
            continue
        initializer = values[cursor + 1:end]
        identifiers = frozenset(
            token for token in initializer if re.fullmatch(r"[A-Za-z_]\w*", token)
        )
        declarations.append((
            alias, reference_kind, identity_target(initializer), identifiers,
            items[index][1], allowed_value_initializer(initializer),
        ))
    return declarations


def gpu_op_scope_names(source):
    names = set()
    protected_types = gpu_op_scope_type_names(source)
    candidate_types = protected_types | {
        type_name.rsplit("::", 1)[-1] for type_name in protected_types
    }
    qualified_type = "|".join(
        re.escape(type_name) for type_name in sorted(candidate_types, key=len, reverse=True)
    )
    for match in re.finditer(
            rf"(?<![A-Za-z0-9_])((?:::)?(?:{qualified_type}))\s*"
            r"(?:const\s*)?(?:[&*]\s*)?"
            r"([A-Za-z_]\w*)\b", source):
        if gpu_op_scope_owner_is_protected(source, match.group(1), match.start()):
            names.add(match.group(2))
    changed = True
    reference_declarations = gpu_op_scope_reference_alias_pairs(source)
    while changed:
        changed = False
        for alias, reference_kind, target, identifiers, _position, _value_allowed \
                in reference_declarations:
            touches_protected = target in names or not identifiers.isdisjoint(names)
            if reference_kind is not None and touches_protected and alias not in names:
                names.add(alias)
                changed = True
    return names


def noncanonical_gpu_op_scope_initializers(source, relative=None):
    names = gpu_op_scope_names(source)
    violations = []
    for alias, reference_kind, target, identifiers, position, value_receiver in \
            gpu_op_scope_reference_alias_pairs(source):
        if identifiers.isdisjoint(names):
            continue
        if reference_kind == "*":
            violations.append((alias, position))
            continue
        if reference_kind is not None and target in names:
            continue
        if (reference_kind is None and value_receiver in names):
            continue
        if (reference_kind is None and value_receiver == "*submitCompactedOwners*"
                and relative == "playback/gpu/gpucompositor.cpp"):
            continue
        violations.append((alias, position))
    return violations


def immediate_receiver_name(items, operator):
    """Resolve only an exact identifier with optional transparent parentheses."""
    cursor = operator - 1
    if cursor < 0:
        return None
    if re.fullmatch(r"[A-Za-z_]\w*", items[cursor][0]):
        return items[cursor][0]
    if items[cursor][0] != ")":
        return None
    depth = 1
    opening = cursor - 1
    while opening >= 0:
        if items[opening][0] == ")":
            depth += 1
        elif items[opening][0] == "(":
            depth -= 1
            if depth == 0:
                break
        opening -= 1
    if opening < 0:
        return None
    values = [value for value, _ in items[opening + 1:cursor]]
    while len(values) >= 2 and values[0] == "(" and values[-1] == ")":
        values = values[1:-1]
    return values[0] if len(values) == 1 and re.fullmatch(r"[A-Za-z_]\w*", values[0]) else None


def audit_gpu_scope_method_directness(source, method, relative):
    """Reject member pointers and macro aliases while ignoring unrelated same-named APIs."""
    if method not in source:
        return
    items = tokens(source)
    scope_names = gpu_op_scope_names(source)
    direct = {position for position, _ in gpu_scope_member_call_sites(source, method)}
    method_aliases = {
        match.group(1) for match in re.finditer(
            rf"(?m)^\s*#\s*define\s+([A-Za-z_]\w*)(?:\s*\(\s*\))?[^\n]*"
            rf"\b{re.escape(method)}\b", source)
    }
    for index, (token, position) in enumerate(items):
        if token not in ({method} | method_aliases) or position in direct:
            continue
        if (index >= 2 and items[index - 1][0] == "::"
                and member_pointer_has_protected_owner(source, items, index - 1)):
            raise AssertionError(
                f"{relative}: GpuOpScope::{method} must be a direct auditable member call")
        operator = index - 1
        if operator >= 0 and items[operator][0] == "template":
            operator -= 1
        if operator >= 1 and items[operator][0] in (".", "->"):
            if immediate_receiver_name(items, operator) in scope_names:
                raise AssertionError(
                    f"{relative}: GpuOpScope::{method} must be a direct auditable member call")

    macro_aliases = {
        match.group(1) for match in re.finditer(
            rf"(?m)^\s*#\s*define\s+([A-Za-z_]\w*)(?:\s*\([^\n]*\))?[^\n]*"
            rf"\b{re.escape(method)}\b",
            source,
        )
    }
    for index, (token, _position) in enumerate(items):
        if token not in macro_aliases:
            continue
        opening = call_open_after_template_id(items, index)
        if opening is None:
            continue
        operator = index - 1
        if operator < 1 or items[operator][0] not in (".", "->"):
            continue
        if immediate_receiver_name(items, operator) in scope_names:
            raise AssertionError(
                f"{relative}: GpuOpScope::{method} macro indirection is forbidden")


def native_submit_adapter_sites(source):
    """Discover every production native-submit adapter, including compacted forwarding."""
    items = tokens(source)
    scope_names = gpu_op_scope_names(source)
    helper_region = None
    helper = re.search(r"\bGpuSubmissionResult\s+submitCompactedOwners\b[^\{]*\{", source,
                       re.DOTALL)
    if helper is not None:
        opening = source.find("{", helper.start(), helper.end())
        helper_region = (helper.start(), matching_character(source, opening, "{", "}"))

    sites = []
    for index, (token, position) in enumerate(items):
        if token == "submit":
            opening = call_open_after_template_id(items, index)
            if opening is None:
                continue
            operator_index = index - 1
            if operator_index >= 0 and items[operator_index][0] == "template":
                operator_index -= 1
            if operator_index < 1 or items[operator_index][0] not in (".", "->"):
                continue
            if immediate_receiver_name(items, operator_index) not in scope_names:
                continue
            arguments = call_arguments(items, opening)
            adapter = single_identifier(arguments[0]) if arguments else None
            require(adapter is not None,
                    "native GpuOpScope::submit must name one auditable local adapter")
            if (helper_region is not None and helper_region[0] <= position <= helper_region[1]
                    and adapter == "adapter"):
                continue
            sites.append((position, adapter))
        elif token == "submitCompactedOwners":
            opening = call_open_after_template_id(items, index)
            if opening is None:
                continue
            if helper_region is not None and helper_region[0] <= position <= helper_region[1]:
                continue
            arguments = call_arguments(items, opening)
            require(len(arguments) >= 2,
                    "compacted native submission must expose its auditable adapter")
            adapter = single_identifier(arguments[1])
            require(adapter is not None,
                    "compacted native submission must name one auditable local adapter")
            sites.append((position, adapter))
    return sites


def local_native_adapter(source, adapter, before, label):
    pattern = re.compile(
        rf"\bauto\s+{re.escape(adapter)}\s*=\s*\[[^\]]*\]\s*"
        r"\(\s*const\s+(?P<type>GpuScopedNativeView\s*<[^>]+>|auto)\s*&\s*"
        r"(?P<view>[A-Za-z_]\w*)\s*\)\s*noexcept\s*\{",
        re.DOTALL,
    )
    definitions = [match for match in pattern.finditer(source, 0, before)]
    require(definitions, f"{label}: native submission adapter must be a local audited lambda")
    match = definitions[-1]
    opening = source.find("{", match.start(), match.end())
    closing = matching_character(source, opening, "{", "}")
    require(closing < before,
            f"{label}: native submission adapter definition must end before its submission")
    return match.group("view"), source[opening:closing + 1]


def macro_definitions(source):
    definitions = {}
    logical = source.replace("\\\r\n", "").replace("\\\n", "")
    for line in logical.splitlines():
        match = re.match(
            r"\s*#\s*define\s+([A-Za-z_]\w*)(\(([^)]*)\))?\s*(.*)$", line)
        if match is None:
            continue
        parameters = None
        if match.group(2) is not None:
            parameters = tuple(
                item.strip() for item in match.group(3).split(",") if item.strip())
        definitions[match.group(1)] = (parameters, match.group(4))
    return definitions


def split_macro_arguments(source, opening):
    arguments = []
    start = opening + 1
    paren = bracket = brace = 0
    for index in range(opening + 1, len(source)):
        char = source[index]
        if char == "(" :
            paren += 1
        elif char == ")":
            if paren == bracket == brace == 0:
                if index != start or arguments:
                    arguments.append(source[start:index])
                return arguments, index
            paren -= 1
        elif char == "[":
            bracket += 1
        elif char == "]":
            bracket -= 1
        elif char == "{":
            brace += 1
        elif char == "}":
            brace -= 1
        elif char == "," and paren == bracket == brace == 0:
            arguments.append(source[start:index])
            start = index + 1
    raise AssertionError("unterminated macro invocation in native submit adapter")


def expand_adapter_macros(body, source):
    """Deterministically expand source-defined macros that occur inside an adapter."""
    definitions = macro_definitions(source)
    expanded = body
    for _depth in range(32):
        changes = []
        for name, (parameters, replacement) in definitions.items():
            if parameters is None:
                for match in re.finditer(rf"\b{re.escape(name)}\b", expanded):
                    changes.append((match.start(), match.end(), replacement))
                continue
            for match in re.finditer(rf"\b{re.escape(name)}\s*\(", expanded):
                opening = expanded.find("(", match.start(), match.end())
                arguments, closing = split_macro_arguments(expanded, opening)
                if len(arguments) != len(parameters):
                    raise AssertionError(
                        "native submit adapter macro arity is not statically auditable")
                result = replacement
                for parameter, argument in zip(parameters, arguments):
                    result = re.sub(rf"\b{re.escape(parameter)}\b", argument, result)
                changes.append((match.start(), closing + 1, result))
        if not changes:
            return expanded
        # Resolve the left-most non-overlapping expansion per pass. Re-scanning
        # provides deterministic nested expansion without exponential growth.
        start, end, replacement = min(changes, key=lambda item: (item[0], item[1]))
        expanded = expanded[:start] + replacement + expanded[end:]
        require(len(expanded) <= max(len(body) * 16, 65536),
                "native submit adapter macro expansion exceeded its bounded audit size")
    raise AssertionError("native submit adapter macro expansion exceeded its bounded depth")


def audit_native_adapter_body(body, view, relative):
    label = f"{relative}: native submission adapter"
    slot = rf"{re.escape(view)}\s*(?:\[[^\]]+\]|\.\s*get\s*<[^>]+>\s*\(\s*\))"
    address_uses = list(re.finditer(rf"&\s*{slot}", body, re.DOTALL))
    reference_alias = re.search(
        rf"\b(?:auto|GpuScopedNativeSurface)\s*(?:const\s*)?&{{1,2}}\s*"
        rf"[A-Za-z_]\w*\s*=\s*{slot}", body, re.DOTALL)
    pointer_alias = re.search(
        rf"\b(?:auto|GpuScopedNativeSurface)\s*(?:const\s*)?\*\s*"
        rf"[A-Za-z_]\w*\s*=\s*&?\s*{slot}", body, re.DOTALL)
    wrapper = re.search(
        rf"\b(?:std\s*::\s*(?:ref|cref)|reference_wrapper)\b[^;]*{slot}",
        body, re.DOTALL)
    returned = re.search(rf"\breturn\b[^;]*{slot}", body, re.DOTALL)

    compositor_accessor = re.compile(
        rf"\bauto\s+nativeAt\s*=\s*\[\s*&\s*\]\s*\(\s*size_t\s+slot\s*\)\s*"
        rf"->\s*const\s+GpuScopedNativeSurface\s*\*\s*\{{\s*return\s+slot\s*<\s*"
        rf"{re.escape(view)}\s*\.\s*size\s*\(\s*\)\s*\?\s*&\s*"
        rf"{re.escape(view)}\s*\[\s*slot\s*\]\s*:\s*nullptr\s*;\s*\}}\s*;",
        re.DOTALL,
    )
    is_exact_compositor = (
        relative == "playback/gpu/gpucompositor.cpp"
        and len(compositor_accessor.findall(body)) == 1
        and len(address_uses) == 1
        and len(re.findall(r"\binvokeOnRenderThread\s*\(", body)) == 1
        and len(re.findall(r"\brenderGridWithRhi\s*\(", body)) == 1
        and re.search(r"\brenderGridWithRhi\s*\([^;]*\bnativeAt\b", body, re.DOTALL)
        is not None
    )
    if is_exact_compositor:
        require(reference_alias is None and pointer_alias is None and wrapper is None,
                f"{label}: compositor scoped accessor must not gain another escape")
        values = [token for token, _ in tokens(body)]
        require(values.count(view) == 2 and values.count("nativeAt") == 2,
                f"{label}: compositor view/accessor must remain confined to the exact render "
                "argument")
        return

    require(not address_uses and reference_alias is None and pointer_alias is None and
            wrapper is None and returned is None,
            f"{label}: scoped native view slots must not escape by pointer or reference")
    values = [token for token, _ in tokens(body)]
    require(values.count(view) == 1 and re.search(
        rf"\bimportAndReadback\s*\(\s*{re.escape(view)}\s*\.\s*get\s*<\s*0\s*>\s*"
        r"\(\s*\)\s*,",
        body,
    ) is not None,
            f"{label}: non-compositor view must flow once to the reviewed exact readback sink")


def audit_native_submit_source(source, relative):
    cleaned = strip_comments_and_literals(source)
    violations = noncanonical_gpu_op_scope_initializers(cleaned, relative)
    require(not violations,
            f"{relative}: non-canonical protected receiver initializer is forbidden")
    audit_gpu_scope_method_directness(cleaned, "submit", relative)
    for position, adapter in native_submit_adapter_sites(cleaned):
        view, body = local_native_adapter(cleaned, adapter, position, relative)
        audit_native_adapter_body(expand_adapter_macros(body, cleaned), view, relative)


def audit_native_submit_production_calls(repo_root):
    for path in first_party_source_paths(repo_root):
        relative = path.relative_to(repo_root).as_posix()
        if relative == "playback/gpu/gpuopscope.h":
            continue
        audit_native_submit_source(path.read_text(encoding="utf-8"), relative)


def audit_retained_only_source(source, relative, forbid=False):
    audit_gpu_scope_method_directness(source, "submitRetained", relative)
    calls = retained_only_call_sites(source)
    if not calls:
        return
    require(not forbid,
            f"retained-only submission is forbidden in native readback/compositor: {relative}")
    for call_start, arguments_start in calls:
        arguments = source[arguments_start:]
        first_argument = re.match(r"\s*([A-Za-z_]\w*)\s*,", arguments)
        require(first_argument is not None,
                f"{relative}: retained-only call must name one auditable local adapter")
        adapter = re.escape(first_argument.group(1))
        bindings = list(re.finditer(
            rf"\b{adapter}\s*=",
            source[:call_start],
        ))
        require(bindings,
                f"{relative}: retained-only adapter must be locally defined")
        binding_start = bindings[-1].start()
        statement_start = max(
            source.rfind(";", 0, binding_start),
            source.rfind("{", 0, binding_start),
            source.rfind("}", 0, binding_start),
            source.rfind("\n", 0, binding_start),
        ) + 1
        nearest = source[statement_start:call_start].lstrip()
        exact_definition = re.match(
            rf"\bauto\s+{adapter}\s*=\s*\[\s*\]\s*\(\s*\)\s*noexcept\s*"
            r"\{\s*return\s+GpuSubmitOutcome\s*::\s*Submitted\s*;\s*\}\s*;",
            nearest,
            re.DOTALL,
        )
        require(exact_definition is not None,
                f"{relative}: retained-only submission must use one local, "
                "capture-free, no-op Submitted adapter")


def audit_retained_only_production_calls(repo_root):
    for path in first_party_source_paths(repo_root):
        source = strip_comments_and_literals(path.read_text(encoding="utf-8"))
        relative = path.relative_to(repo_root).as_posix()
        if relative == "playback/gpu/gpuopscope.h":
            continue
        audit_retained_only_source(
            source, relative, relative in RETAINED_ONLY_FORBIDDEN_PATHS)


def audit_pending_fence_production(repo_root):
    surface_header = (repo_root / "playback/gpu/gpusurface.h").read_text(encoding="utf-8")
    audit_fixed_surface_watermark(surface_header, "GpuSurface fixed pending-fence contract")
    forbidden = []
    for path in first_party_source_paths(repo_root):
        source = strip_comments_and_literals(path.read_text(encoding="utf-8"))
        if path.resolve() == (repo_root / "playback/gpu/gpusurface.h").resolve():
            continue
        derives_surface = re.search(
            r"\bclass\s+[A-Za-z_]\w*[^;{]*:\s*[^\n{]*\bGpuSurface\b",
            source,
        )
        defines_override = re.search(
            r"\b(?:retainUntilFenceRetired|pendingFenceValue)\s*\([^)]*\)\s*"
            r"(?:const\s*)?override\b",
            source,
        )
        defines_out_of_line = re.search(
            r"\b(?:void|uint64_t)\s+[A-Za-z_]\w*\s*::\s*"
            r"(?:retainUntilFenceRetired|pendingFenceValue)\s*\(",
            source,
        )
        duplicates_watermark = derives_surface and re.search(
            r"std\s*::\s*atomic\s*<\s*uint64_t\s*>\s+"
            r"[A-Za-z_]\w*(?:pending|fence)[A-Za-z_0-9]*",
            source,
            re.IGNORECASE,
        )
        if defines_override or defines_out_of_line or duplicates_watermark:
            forbidden.append(path)
    require(not forbidden,
            "derived production surfaces must not override or duplicate the fixed pending-fence "
            "watermark: " + ", ".join(str(path) for path in forbidden))
    submission = (repo_root / "playback/gpu/gpusubmission.h").read_text(encoding="utf-8")
    audit_two_field_surface_compatibility(submission, "GpuSurfaceCompatibility ABI")
    op_scope = (repo_root / "playback/gpu/gpuopscope.h").read_text(encoding="utf-8")
    audit_retained_only_submission_api(op_scope, "GpuOpScope retained-only API")
    audit_publish_then_stamp(function_block(op_scope, "GpuSubmissionResult submitImpl"),
                             "GpuOpScope publish-then-stamp ordering")
    audit_retained_only_production_calls(repo_root)
    audit_native_submit_production_calls(repo_root)


def fixed_pending_fence_mutation_self_tests():
    safe_retained_api = """
template <typename BackendAdapter, size_t N>
GpuSubmissionResult submitRetained(BackendAdapter& adapter, GpuSurfacePack<N> surfaces) noexcept {
    static_assert(std::is_nothrow_invocable_r_v<GpuSubmitOutcome, BackendAdapter&>);
    return submitImpl<false>(adapter, std::move(surfaces));
}
template <bool ExposeNative, typename BackendAdapter, size_t N>
GpuSubmissionResult submitImpl(BackendAdapter& adapter, GpuSurfacePack<N> surfaces) noexcept {
    using ScopedView = GpuScopedNativeView<N>;
    auto submitScoped = [&](const ScopedView* view) {
        if constexpr (ExposeNative)
            outcome = std::invoke(adapter, *view);
        else
            outcome = std::invoke(adapter);
    };
    if constexpr (ExposeNative) {
        auto invokeScoped = [&](const ScopedView& view) { submitScoped(&view); };
        withScopedNativeView(surfaces, invokeScoped);
    } else {
        submitScoped(nullptr);
    }
}
"""
    audit_retained_only_submission_api(safe_retained_api,
                                       "safe retained-only API mutation")
    for mutation, message in (
        (safe_retained_api.replace(
            "GpuSubmitOutcome, BackendAdapter&>",
            "GpuSubmitOutcome, BackendAdapter&, const GpuScopedNativeSurface&>"),
         "a retained adapter accepting scoped capability must be rejected"),
        (safe_retained_api.replace("return submitImpl<false>", "return submitImpl<true>"),
         "retained API selecting native submission must be rejected"),
        (safe_retained_api.replace("outcome = std::invoke(adapter);",
                                   "outcome = std::invoke(adapter, *view);"),
         "retained branch passing a scoped view must be rejected"),
        (safe_retained_api.replace("submitScoped(nullptr);",
                                   "withScopedNativeView(surfaces, invokeScoped);"),
         "retained branch constructing a scoped view must be rejected"),
    ):
        try:
            audit_retained_only_submission_api(mutation, "retained-only API mutation")
        except AssertionError:
            continue
        raise AssertionError(message)

    safe_retained_call = """
void mint() {
    GpuOpScope operation(fence, registry);
    auto adapter = []() noexcept { return GpuSubmitOutcome::Submitted; };
    operation.submitRetained(adapter, GpuSurfacePack<1>({surface}));
}
"""
    audit_retained_only_source(safe_retained_call, "safe_retained.cpp")
    for mutation, forbidden, message in (
        (safe_retained_call.replace("[]()", "[&]()"), False,
         "a retained-only adapter with captures must be rejected"),
        (safe_retained_call.replace(
            "return GpuSubmitOutcome::Submitted;",
            "readback(surface.nativeHandle()); return GpuSubmitOutcome::Submitted;"), False,
         "a retained-only adapter performing native work must be rejected"),
        (safe_retained_call.replace(" noexcept", ""), False,
         "a potentially throwing retained-only adapter must be rejected"),
        (safe_retained_call.replace(
            "adapter, GpuSurfacePack<1>({surface})",
            "[]() noexcept { return GpuSubmitOutcome::Submitted; }, "
            "GpuSurfacePack<1>({surface})"), False,
         "an inline retained adapter that evades local audit must be rejected"),
        (safe_retained_call, True,
         "a retained-only submission in a native readback/compositor path must be rejected"),
        (safe_retained_call.replace(
            "    operation.submitRetained",
            "    auto adapter = [&]() noexcept { return GpuSubmitOutcome::Submitted; };\n"
            "    operation.submitRetained"), False,
         "an earlier safe decoy must not hide the adapter nearest the retained call"),
        (safe_retained_call.replace(
            "    operation.submitRetained(adapter, GpuSurfacePack<1>({surface}));",
            "    {\n"
            "        auto unsafe = [&]() noexcept { useNative(surface); "
            "return GpuSubmitOutcome::Submitted; };\n"
            "        auto& adapter = unsafe;\n"
            "        operation.submitRetained(adapter, GpuSurfacePack<1>({surface}));\n"
            "    }"), False,
         "a shadowing adapter reference must not reuse an earlier safe decoy"),
        (safe_retained_call.replace(
            "    operation.submitRetained(adapter, GpuSurfacePack<1>({surface}));",
            "    operation.submitRetained(adapter, GpuSurfacePack<1>({surface}));\n"
            "    auto capturedAdapter = [&]() noexcept { useNative(surface); "
            "return GpuSubmitOutcome::Submitted; };\n"
            "    (((operation))).submitRetained<1>(capturedAdapter, "
            "GpuSurfacePack<1>({surface}));"), False,
         "an explicit-template retained call through a parenthesized member must be audited"),
        (safe_retained_call.replace(
            "    operation.submitRetained(adapter, GpuSurfacePack<1>({surface}));",
            "    operation.submitRetained(adapter, GpuSurfacePack<1>({surface}));\n"
            "    auto rawAdapter = []() noexcept { readNative(globalSurface); "
            "return GpuSubmitOutcome::Submitted; };\n"
            "    operation.template submitRetained<GpuSurfacePack<1>>(rawAdapter, "
            "GpuSurfacePack<1>({surface}));"), False,
         "a dependent explicit-template retained call must not hide a raw-work adapter"),
        (safe_retained_call.replace(
            "    operation.submitRetained(adapter, GpuSurfacePack<1>({surface}));",
            "    UnsafeRetainedAdapter unsafe;\n"
            "    auto member = &GpuOpScope::submitRetained<UnsafeRetainedAdapter, 1>;\n"
            "    (operation.*member)(unsafe, GpuSurfacePack<1>({surface}));"), False,
         "a retained-only member pointer must not bypass direct-call enforcement"),
        (safe_retained_call.replace(
            "    operation.submitRetained(adapter, GpuSurfacePack<1>({surface}));",
            "#define RETAINED_CALL submitRetained\n"
            "    operation.RETAINED_CALL(adapter, GpuSurfacePack<1>({surface}));"), False,
         "an object-like retained-only member alias must be rejected"),
    ):
        try:
            audit_retained_only_source(mutation, "retained-only call mutation", forbidden)
        except AssertionError:
            continue
        raise AssertionError(message)

    retained_decoy = """
#define RETAINED_CALL submitRetained
struct Queue { void submitRetained(int); };
void decoy(GpuOpScope& operation, Queue& queue) {
    consume(operation, queue.RETAINED_CALL(1));
}
"""
    audit_retained_only_source(retained_decoy, "project/retained_decoy.cpp")
    for declaration in (
        "using ScopeAlias = GpuOpScope;",
        "typedef GpuOpScope ScopeAlias;",
        "using FirstScope = GpuOpScope; using ScopeAlias = FirstScope;",
        "using ScopeAlias = ::GpuOpScope;",
        "namespace gpu { using FirstScope = ::GpuOpScope; } "
        "using ScopeAlias = gpu::FirstScope;",
    ):
        mutation = safe_retained_call.replace(
            "void mint() {", declaration + "\nvoid mint() {").replace(
            "    operation.submitRetained(adapter, GpuSurfacePack<1>({surface}));",
            "    auto member = &ScopeAlias::submitRetained<decltype(adapter), 1>;\n"
            "    consume(member);")
        try:
            audit_retained_only_source(mutation, "retained alias member pointer")
        except AssertionError:
            continue
        raise AssertionError(
            "a GpuOpScope type alias must not hide a submitRetained member pointer")

    unrelated_qualified_type = """
namespace ordinary { struct GpuOpScope {}; }
using ScopeAlias = ordinary::GpuOpScope;
void decoy() { auto member = &ScopeAlias::submitRetained<Adapter, 1>; consume(member); }
"""
    audit_gpu_scope_method_directness(
        unrelated_qualified_type, "submitRetained", "project/unrelated_retained_alias.cpp")
    for source in (
        "void bad() { auto member = &::GpuOpScope::submitRetained<Adapter, 1>; }",
        "namespace gpu { using ScopeAlias = ::GpuOpScope; } "
        "void bad() { auto member = &gpu::ScopeAlias::submitRetained<Adapter, 1>; }",
        "#define SCOPE_OWNER ::GpuOpScope\n"
        "void bad() { auto member = &SCOPE_OWNER::submitRetained<Adapter, 1>; }",
        "#define RETAINED_MEMBER() submitRetained\n"
        "namespace gpu { using ScopeAlias = ::GpuOpScope; } "
        "void bad() { auto member = &gpu::ScopeAlias::RETAINED_MEMBER()<Adapter, 1>; }",
    ):
        try:
            audit_gpu_scope_method_directness(
                source, "submitRetained", "qualified retained pointer")
        except AssertionError:
            continue
        raise AssertionError("a qualified or macro submitRetained member pointer was accepted")

    safe_compatibility = """
struct GpuSurfaceCompatibility {
    uintptr_t deviceDomainId = 0;
    uint64_t authorityEpoch = 0;
};
"""
    audit_two_field_surface_compatibility(safe_compatibility,
                                          "safe compatibility POD mutation")
    for mutation, message in (
        (safe_compatibility.replace("};", "    uint64_t stampCarrier = 0;\n};"),
         "a third compatibility field must be rejected"),
        (safe_compatibility.replace("uint64_t authorityEpoch", "uintptr_t authorityEpoch"),
         "changing the authority field type must be rejected"),
        (safe_compatibility.replace("uint64_t authorityEpoch = 0;",
                                    "GpuPendingFenceStamp pendingFenceStamp;"),
         "a capability stamp carrier must be rejected"),
    ):
        try:
            audit_two_field_surface_compatibility(mutation, "compatibility POD mutation")
        except AssertionError:
            continue
        raise AssertionError(message)

    safe_surface = """
class GpuSurface {
public:
    void retainUntilFenceRetired(uint64_t fenceValue) noexcept {
        uint64_t previous = m_pendingFence.load(std::memory_order_relaxed);
        while (fenceValue > previous && !m_pendingFence.compare_exchange_weak(
                previous, fenceValue, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
        }
    }
    uint64_t pendingFenceValue() const noexcept {
        return m_pendingFence.load(std::memory_order_relaxed);
    }
private:
    mutable std::atomic<uint64_t> m_pendingFence{0};
};
"""
    audit_fixed_surface_watermark(safe_surface, "safe fixed watermark mutation")
    for mutation, message in (
        (safe_surface.replace("!m_pendingFence.compare_exchange_weak",
                              "!m_otherFence.compare_exchange_weak"),
         "retain using another watermark must be rejected"),
        (safe_surface.replace("return m_pendingFence.load", "return m_otherFence.load"),
         "pendingFenceValue using another watermark must be rejected"),
        (safe_surface.replace("    void retainUntilFenceRetired",
                              "    virtual void retainUntilFenceRetired"),
         "a virtual retain operation must be rejected"),
        (safe_surface.replace("    uint64_t pendingFenceValue",
                              "    virtual uint64_t pendingFenceValue"),
         "a virtual pending query must be rejected"),
        (safe_surface.replace(" noexcept {", " {", 1),
         "a potentially-throwing retain operation must be rejected"),
        (safe_surface.replace("public:\n    void retainUntilFenceRetired",
                              "protected:\n    void retainUntilFenceRetired"),
         "a protected retain operation must be rejected"),
        (safe_surface.replace("memory_order_relaxed", "memory_order_acquire", 1),
         "using the value-only watermark as an acquire edge must be rejected"),
        (safe_surface.replace("private:\n    mutable std::atomic",
                              "protected:\n    mutable std::atomic"),
         "a non-private fixed watermark must be rejected"),
    ):
        try:
            audit_fixed_surface_watermark(mutation, "fixed watermark mutation")
        except AssertionError:
            continue
        raise AssertionError(message)

    safe_submit = """
GpuSubmissionResult submit() {
    const std::shared_ptr<GpuSurface>* retirementOwners = surfaces.m_surfaces.data();
    if (uniqueCount != N) {
        for (size_t i = 0; i < uniqueCount; ++i)
            (*coalescedOwners)[i] = surfaces.m_surfaces[uniqueIndices[i]];
        retirementOwners = coalescedOwners->data();
    }
    auto prepared = m_registry.prepareRetirement(
        retirementOwners, qsizetype(uniqueCount), m_fence);
    if (exact && m_registry.publishPrepared(prepared, std::move(ticket))) {
        for (size_t i = 0; i < uniqueCount; ++i) {
            retirementOwners[i]->retainUntilFenceRetired(ticketValue);
        }
        result.retirement = GpuRetirementDisposition::Published;
    } else {
        m_registry.quarantinePrepared(prepared);
    }
    return result;
}
"""
    audit_publish_then_stamp(safe_submit, "safe publish-then-stamp mutation")
    for mutation, message in (
        (safe_submit.replace(
            "    if (exact && m_registry.publishPrepared(prepared, std::move(ticket))) {",
            "    retirementOwners[0]->retainUntilFenceRetired(ticketValue);\n"
            "    if (exact && m_registry.publishPrepared(prepared, std::move(ticket))) {"),
         "stamp before successful publication must be rejected"),
        (safe_submit.replace(
            "        m_registry.quarantinePrepared(prepared);",
            "        retirementOwners[0]->retainUntilFenceRetired(ticketValue);\n"
            "        m_registry.quarantinePrepared(prepared);"),
         "stamp on publication failure must be rejected"),
        (safe_submit.replace(
            "        for (size_t i = 0; i < uniqueCount; ++i) {",
            "        if (false) {\n"
            "        for (size_t i = 0; i < uniqueCount; ++i) {").replace(
                "        }\n        result.retirement", "        }\n        }\n        result.retirement"),
         "an unreachable dummy stamp after publication must be rejected"),
        (safe_submit.replace("i < uniqueCount", "i < 1"),
         "stamping fewer than every unique retained owner must be rejected"),
        (safe_submit.replace("retirementOwners[i]->retain", "surfaces.m_surfaces[i]->retain"),
         "stamping a positional rather than exact unique owner must be rejected"),
        (safe_submit.replace("retainUntilFenceRetired(ticketValue)",
                             "retainUntilFenceRetired(ticketValue + 1)"),
         "stamping a value other than the published ticket must be rejected"),
        (safe_submit.replace(
            "retirementOwners, qsizetype(uniqueCount), m_fence",
            "surfaces.m_surfaces.data(), qsizetype(uniqueCount), m_fence"),
         "retaining an owner array different from the stamped array must be rejected"),
        (safe_submit.replace("m_surfaces[uniqueIndices[i]]", "m_surfaces[i]"),
         "coalescing positional rather than exact unique owners must be rejected"),
    ):
        try:
            audit_publish_then_stamp(mutation, "publish-then-stamp mutation")
        except AssertionError:
            continue
        raise AssertionError(message)


def exact_retained_owner_mutation_self_tests():
    safe_readback = """
GpuReadbackResult submitGpuReadback(const std::shared_ptr<GpuRhiContext>& rhi,
                                    const std::shared_ptr<GpuSurface>& surface,
                                    FramePixelFormat target) noexcept {
    GpuReadbackResult readback;
    GpuRetireRegistry registry;
    GpuOpScope operation(readbackFence, registry);
    auto adapter = [&](const GpuScopedNativeView<1>& view) noexcept {
        readback = rhi->importAndReadback(view.get<0>(), target);
        return readback.outcome;
    };
    (void) operation.submit(adapter, GpuSurfacePack<1>({surface}));
    return readback;
}
"""
    audit_exact_readback_owner_binding(safe_readback, "safe exact readback mutation")
    rejected_readbacks = (
        (safe_readback.replace("view.get<0>()", "surface"),
         "readback through a captured retained owner must be rejected"),
        (safe_readback.replace(
            "readback = rhi->importAndReadback(view.get<0>(), target);",
            "if (useScoped)\n"
            "            readback = rhi->importAndReadback(view.get<0>(), target);\n"
            "        else\n"
            "            readback = backendReadback(surface, target);",
        ), "a conditional correct path plus captured fallback must be rejected"),
        (safe_readback.replace(
            "readback = rhi->importAndReadback(view.get<0>(), target);",
            "auto neverCalled = [&] {\n"
            "            readback = rhi->importAndReadback(view.get<0>(), target);\n"
            "        };\n"
            "        readback = backendReadback(surface, target);",
        ), "a scoped readback hidden in an uninvoked lambda must be rejected"),
        (safe_readback.replace(
            "readback = rhi->importAndReadback(view.get<0>(), target);",
            "escapedView = &view;\n"
            "        readback = backendReadback(surface, target);",
        ), "a scoped native view escape must be rejected"),
        (safe_readback.replace(
            "readback = rhi->importAndReadback(view.get<0>(), target);",
            "escapedHandle = view.get<0>().nativeHandle();\n"
            "        readback = backendReadback(surface, target);",
        ), "a raw handle escape from the adapter must be rejected"),
        (safe_readback.replace(
            "view.get<0>()", "useScoped ? view.get<0>() : recoverScopedSurface(surface)"),
         "a conditional exact slot with a decoy retained owner must be rejected"),
    )
    for mutation, message in rejected_readbacks:
        try:
            audit_exact_readback_owner_binding(mutation, "exact readback mutation")
        except AssertionError:
            continue
        raise AssertionError(message)

    safe_compositor = """
FrameHandle compose() {
    auto adapter = [&](const auto& nativeView) noexcept {
        const bool invoked = rhi->invokeOnRenderThread([&](QRhi* renderRhi) {
            auto nativeAt = [&](size_t slot) -> const GpuScopedNativeSurface* {
                return slot < nativeView.size() ? &nativeView[slot] : nullptr;
            };
            renderResult = renderGridWithRhi(renderRhi, sources, nativeAt, outputSlot);
        });
        if (!invoked || !renderResult.submissionAttempted)
            return GpuSubmitOutcome::NotSubmitted;
        return GpuSubmitOutcome::Submitted;
    };
    return submitCompactedOwners<1>(operation, adapter, owners, ownerCount);
}
"""
    audit_exact_compositor_owner_binding(safe_compositor, "safe exact compositor mutation")
    rejected_compositors = (
        (safe_compositor.replace(
            "return slot < nativeView.size() ? &nativeView[slot] : nullptr;",
            "return slot == capturedSlot ? &capturedNativeSurface : nullptr;"),
         "a compositor accessor recovering a captured surface must be rejected"),
        (safe_compositor.replace(
            "auto nativeAt = [&](size_t slot) -> const GpuScopedNativeSurface* {\n"
            "                return slot < nativeView.size() ? &nativeView[slot] : nullptr;\n"
            "            };",
            "auto neverCalled = [&] {\n"
            "                auto nativeAt = [&](size_t slot) -> const GpuScopedNativeSurface* {\n"
            "                    return slot < nativeView.size() ? &nativeView[slot] : nullptr;\n"
            "                };\n"
            "            };\n"
            "            auto capturedAt = [&](size_t) { return &capturedNativeSurface; };"),
         "a correct accessor hidden in an uninvoked lambda must be rejected"),
        (safe_compositor.replace("sources, nativeAt, outputSlot",
                                 "sources, capturedAt, outputSlot"),
         "the render primitive must consume the accessor derived from the scoped view"),
        (safe_compositor.replace(
            "return slot < nativeView.size() ? &nativeView[slot] : nullptr;",
            "return useScoped ? &nativeView[slot] : &capturedNativeSurface;"),
         "a conditional scoped accessor with a captured alternative must be rejected"),
        (safe_compositor.replace(
            "const bool invoked = rhi->invokeOnRenderThread",
            "escapedView = &nativeView;\n"
            "        const bool invoked = rhi->invokeOnRenderThread"),
         "a compositor scoped view escape must be rejected"),
        (safe_compositor.replace(
            "renderResult = renderGridWithRhi",
            "escapedHandle = nativeView[0].nativeHandle();\n"
            "            renderResult = renderGridWithRhi"),
         "a compositor raw handle escape must be rejected"),
        (safe_compositor.replace(
            "renderResult = renderGridWithRhi",
            "auto* escapedSlot = &nativeView[0];\n"
            "            queue([=] { consume(escapedSlot); });\n"
            "            renderResult = renderGridWithRhi"),
         "a scoped native slot queued beyond the adapter must be rejected"),
        (safe_compositor.replace(
            "renderResult = renderGridWithRhi",
            "globalSlot = &nativeView[0];\n"
            "            renderResult = renderGridWithRhi"),
         "a compositor native slot pointer must not escape to global storage"),
        (safe_compositor.replace(
            "renderResult = renderGridWithRhi",
            "const auto& slotAlias = nativeView[0];\n"
            "            member.slot = &slotAlias;\n"
            "            renderResult = renderGridWithRhi"),
         "a compositor native slot reference alias must not escape to member storage"),
        (safe_compositor.replace(
            "renderResult = renderGridWithRhi",
            "escapedSlots.push_back(&nativeView[0]);\n"
            "            renderResult = renderGridWithRhi"),
         "a compositor native slot must not escape through a container"),
        (safe_compositor.replace(
            "renderResult = renderGridWithRhi",
            "auto wrapped = std::cref(nativeView[0]);\n"
            "            store(wrapped);\n"
            "            renderResult = renderGridWithRhi"),
         "a compositor native slot must not escape through a reference wrapper"),
        (safe_compositor.replace(
            "renderResult = renderGridWithRhi(renderRhi, sources, nativeAt, outputSlot);",
            "if (false)\n"
            "                renderResult = renderGridWithRhi(renderRhi, sources, nativeAt, "
            "outputSlot);\n"
            "            renderResult = renderGridWithRhi(renderRhi, sources, capturedAt, "
            "outputSlot);"),
         "a dummy correct render path plus an unsafe real path must be rejected"),
    )
    for mutation, message in rejected_compositors:
        try:
            audit_exact_compositor_owner_binding(mutation, "exact compositor mutation")
        except AssertionError:
            continue
        raise AssertionError(message)

    safe_backend = """
CVPixelBufferRef makeWrapper(const GpuScopedNativeSurface& surface) {
    if (!surface.valid()) return nullptr;
    return retainApplePixelBufferWrapper(surface);
}
"""
    audit_scoped_surface_backend(safe_backend, "CVPixelBufferRef makeWrapper",
                                 "safe scoped backend mutation",
                                 ("retainApplePixelBufferWrapper",))
    rejected_backends = (
        (safe_backend.replace("retainApplePixelBufferWrapper(surface)",
                              "retainApplePixelBufferWrapper(globalSurface)"),
         "a backend helper recovering a global surface must be rejected"),
        (safe_backend.replace(
            "return retainApplePixelBufferWrapper(surface);",
            "auto neverCalled = [&] { return retainApplePixelBufferWrapper(surface); };\n"
            "    return retainApplePixelBufferWrapper(sharedSurface);"),
         "a deferred exact helper plus shared-surface recovery must be rejected"),
        (safe_backend.replace(
            "return retainApplePixelBufferWrapper(surface);",
            "escapedSurface = &surface;\n"
            "    return retainApplePixelBufferWrapper(surface);"),
         "a backend scoped-surface escape must be rejected"),
        (safe_backend.replace("const GpuScopedNativeSurface& surface",
                              "const std::shared_ptr<GpuSurface>& surface"),
         "a backend shared owner parameter must be rejected"),
    )
    for mutation, message in rejected_backends:
        try:
            audit_scoped_surface_backend(mutation, "CVPixelBufferRef makeWrapper",
                                         "scoped backend mutation",
                                         ("retainApplePixelBufferWrapper",))
        except AssertionError:
            continue
        raise AssertionError(message)

    safe_native_backend = """
void* importNative(const GpuScopedNativeSurface& surface) {
    void* nativeHandle = surface.nativeHandle();
    return consumeImmediately(nativeHandle);
}
"""
    audit_scoped_surface_backend(safe_native_backend, "void* importNative",
                                 "safe native backend mutation",
                                 allow_direct_native_handle=True)
    escaped_native_backend = safe_native_backend.replace(
        "void* nativeHandle = surface.nativeHandle();",
        "escapedHandle = surface.nativeHandle();")
    try:
        audit_scoped_surface_backend(escaped_native_backend, "void* importNative",
                                     "escaped native backend mutation",
                                     allow_direct_native_handle=True)
    except AssertionError:
        pass
    else:
        raise AssertionError("a backend raw native handle escape must be rejected")
    deferred_native_backend = safe_native_backend.replace(
        "return consumeImmediately(nativeHandle);",
        "queue([=] { consume(nativeHandle); });\n"
        "    return nullptr;")
    try:
        audit_scoped_surface_backend(deferred_native_backend, "void* importNative",
                                     "deferred native backend mutation",
                                     allow_direct_native_handle=True)
    except AssertionError:
        pass
    else:
        raise AssertionError("a backend raw handle queued beyond the call must be rejected")


def scoped_readback_boundary_mutation_self_tests():
    safe = """
CpuPlanes fallback() {
    return submitGpuReadback(rhi, surface, FramePixelFormat::Yuv420p).planes;
}
"""
    audit_scoped_readback_caller(safe, "safe scoped readback mutation", True)
    test_authority = """
#ifdef OLR_UNIT_TEST
return context->importAndReadback(surface, target);
#endif
"""
    require(RAW_READBACK_CALL_RE.search(strip_comments_and_literals(
                production_preprocessor_view(test_authority))) is None,
            "unit-test-only raw authority must be absent from the production view")
    mixed_production_branch = """
#if defined(__APPLE__) || defined(OLR_UNIT_TEST)
return context->importAndReadback(surface, target);
#endif
"""
    require(RAW_READBACK_CALL_RE.search(strip_comments_and_literals(
                production_preprocessor_view(mixed_production_branch))) is not None,
            "mixed production/unit branches must remain visible to the production audit")
    negated_mixed_production_branch = """
#if defined(__APPLE__) && !defined(OLR_UNIT_TEST)
return context->importAndReadback(surface, target);
#endif
"""
    require(RAW_READBACK_CALL_RE.search(strip_comments_and_literals(
                production_preprocessor_view(negated_mixed_production_branch))) is not None,
            "negated unit conditions mixed with production macros must remain visible")
    parenthesized_negated_production_branch = """
#if !(defined(OLR_UNIT_TEST))
return context->importAndReadback(surface, target);
#endif
"""
    require(RAW_READBACK_CALL_RE.search(strip_comments_and_literals(
                production_preprocessor_view(parenthesized_negated_production_branch))) is not None,
            "parenthesized negated unit conditions must remain visible")
    equality_production_branch = """
#if defined(OLR_UNIT_TEST) == 0
return context->importAndReadback(surface, target);
#endif
"""
    require(RAW_READBACK_CALL_RE.search(strip_comments_and_literals(
                production_preprocessor_view(equality_production_branch))) is not None,
            "unit-test equality-to-zero conditions must remain visible")
    mixed_boolean_production_branch = """
#if defined(OLR_UNIT_TEST) && 0 || !defined(OLR_UNIT_TEST)
return context->importAndReadback(surface, target);
#endif
"""
    require(RAW_READBACK_CALL_RE.search(strip_comments_and_literals(
                production_preprocessor_view(mixed_boolean_production_branch))) is not None,
            "mixed Boolean unit conditions must remain visible when production can enter")
    unrecognized_similar_macro = """
#ifdef OLR_UNIT_TEST_EXTRA
return context->importAndReadback(surface, target);
#endif
"""
    require(RAW_READBACK_CALL_RE.search(strip_comments_and_literals(
                production_preprocessor_view(unrecognized_similar_macro))) is not None,
            "unrecognized conditionals must remain visible")
    for mutation, message in (
        (safe.replace(
            "submitGpuReadback(rhi, surface, FramePixelFormat::Yuv420p)",
            "rhi->importAndReadback(surface, FramePixelFormat::Yuv420p)",
        ), "direct readback bypass must be rejected"),
        ("""
CpuPlanes fallback() {
    if (false) submitGpuReadback(rhi, surface, FramePixelFormat::Yuv420p);
    auto result = rhi->importAndReadback(surface, FramePixelFormat::Yuv420p);
    return result.planes;
}
""", "unreachable helper plus split direct readback must be rejected"),
        (safe.replace("submitGpuReadback", "otherReadback"),
         "missing scoped readback helper must be rejected"),
        (safe.replace(
            "return submitGpuReadback(rhi, surface, FramePixelFormat::Yuv420p).planes;",
            "// rhi->importAndReadback(surface, target)\n"
            "    const char* marker = \"submitGpuReadback\";\n"
            "    return otherReadback(marker);",
        ), "comment and string markers must not satisfy the readback audit"),
    ):
        try:
            audit_scoped_readback_caller(mutation, "scoped readback mutation", True)
        except AssertionError:
            continue
        raise AssertionError(message)

    safe_helper = """
GpuReadbackResult submitGpuReadback(const std::shared_ptr<GpuRhiContext>& rhi,
                                    const std::shared_ptr<GpuSurface>& surface,
                                    FramePixelFormat target) noexcept {
    GpuReadbackResult readback;
    GpuRetireRegistry registry;
    GpuOpScope operation(readbackFence, registry);
    auto adapter = [&]() noexcept {
        readback = rhi->importAndReadback(surface, target);
        return readback.outcome;
    };
    (void) operation.submit(adapter, surfaces);
    return readback;
}
"""
    audit_scoped_readback_helper(safe_helper, "safe scoped helper mutation")
    dead_scope_after_raw_return = """
GpuReadbackResult submitGpuReadback(const std::shared_ptr<GpuRhiContext>& rhi,
                                    const std::shared_ptr<GpuSurface>& surface,
                                    FramePixelFormat target) noexcept {
    GpuReadbackResult readback = rhi->importAndReadback(surface, target);
    return readback;
    GpuRetireRegistry registry;
    GpuOpScope operation(readbackFence, registry);
    auto adapter = [&]() noexcept { return readback.outcome; };
    (void) operation.submit(adapter, surfaces);
}
"""
    try:
        audit_scoped_readback_helper(dead_scope_after_raw_return,
                                     "dead scoped helper mutation")
    except AssertionError:
        pass
    else:
        raise AssertionError("dead scope after a raw-readback return must be rejected")

    dead_nested_scope_after_raw_return = """
GpuReadbackResult submitGpuReadback(const std::shared_ptr<GpuRhiContext>& rhi,
                                    const std::shared_ptr<GpuSurface>& surface,
                                    FramePixelFormat target) noexcept {
    GpuReadbackResult readback;
    return readback;
    if (true) {
        GpuRetireRegistry registry;
        GpuOpScope operation(readbackFence, registry);
        auto adapter = [&]() noexcept {
            readback = rhi->importAndReadback(surface, target);
            return readback.outcome;
        };
        (void) operation.submit(adapter, surfaces);
    }
}
"""
    try:
        audit_scoped_readback_helper(dead_nested_scope_after_raw_return,
                                     "dead nested scoped helper mutation")
    except AssertionError:
        pass
    else:
        raise AssertionError("dead nested scope after an ancestor return must be rejected")

    dead_scope_after_throw = safe_helper.replace(
        "    GpuReadbackResult readback;",
        "    GpuReadbackResult readback;\n"
        "    throw 0;",
    )
    try:
        audit_scoped_readback_helper(dead_scope_after_throw,
                                     "dead scoped helper after throw mutation")
    except AssertionError:
        pass
    else:
        raise AssertionError("scope evidence after an unconditional throw must be rejected")

    dummy_submit = safe_helper.replace("operation.submit(adapter, surfaces)",
                                       "dummy.submit(adapter, surfaces)")
    try:
        audit_scoped_readback_helper(dummy_submit, "dummy scoped helper mutation")
    except AssertionError:
        pass
    else:
        raise AssertionError("adapter submission through a dummy object must be rejected")

    nested_uninvoked_raw_readback = safe_helper.replace(
        "        readback = rhi->importAndReadback(surface, target);",
        "        auto neverCalled = [&] {\n"
        "            readback = rhi->importAndReadback(surface, target);\n"
        "        };",
    )
    try:
        audit_scoped_readback_helper(nested_uninvoked_raw_readback,
                                     "nested uninvoked raw readback mutation")
    except AssertionError:
        pass
    else:
        raise AssertionError("raw readback hidden in an uninvoked lambda must be rejected")

    safe_fallback = """
int64_t PlaybackWorker::decodePacketIntoBank() {
    auto handleSurface = [&](void* imageBuffer, qint64 pts90k) {
        auto surface = wrapAppleImageBuffer(imageBuffer);
        auto cpuFallback = [surface, gpuRhi]() -> CpuPlanes {
            return submitGpuReadback(gpuRhi, surface, FramePixelFormat::Yuv420p).planes;
        };
        GpuMintResult mint = mintGpuOrDegrade(surface, gpuRhi, meta, renderFence, cpuFallback);
        return true;
    };
    return decoder->decodeKeepSurface(unit, handleSurface, &error);
}
"""
    audit_apple_readback_fallback(safe_fallback, "safe Apple fallback mutation")
    dead_helper = safe_fallback.replace(
        "renderFence, cpuFallback",
        "renderFence, [] { return CpuPlanes{}; }",
    )
    try:
        audit_apple_readback_fallback(dead_helper, "dead Apple fallback mutation")
    except AssertionError:
        pass
    else:
        raise AssertionError("named helper fallback must be passed to mintGpuOrDegrade")

    wrong_surface_consumer = safe_fallback.replace(
        "decodeKeepSurface(unit, handleSurface, &error)",
        "decodeKeepSurface(unit, otherSurface, &error)",
    )
    try:
        audit_apple_readback_fallback(wrong_surface_consumer,
                                      "wrong Apple surface consumer mutation")
    except AssertionError:
        pass
    else:
        raise AssertionError("decodeKeepSurface must consume the audited handleSurface callback")

    unreachable_fallback_helper = safe_fallback.replace(
        "            return submitGpuReadback(gpuRhi, surface, FramePixelFormat::Yuv420p).planes;",
        "            return CpuPlanes{};\n"
        "            return submitGpuReadback(gpuRhi, surface, FramePixelFormat::Yuv420p).planes;",
    )
    try:
        audit_apple_readback_fallback(unreachable_fallback_helper,
                                      "unreachable fallback helper mutation")
    except AssertionError:
        pass
    else:
        raise AssertionError("fallback helper after an unconditional return must be rejected")

    nested_uninvoked_fallback_helper = safe_fallback.replace(
        "            return submitGpuReadback(gpuRhi, surface, FramePixelFormat::Yuv420p).planes;",
        "            auto neverCalled = [&] {\n"
        "                return submitGpuReadback(gpuRhi, surface, "
        "FramePixelFormat::Yuv420p).planes;\n"
        "            };\n"
        "            return CpuPlanes{};",
    )
    try:
        audit_apple_readback_fallback(nested_uninvoked_fallback_helper,
                                      "nested uninvoked fallback helper mutation")
    except AssertionError:
        pass
    else:
        raise AssertionError("fallback helper hidden in an uninvoked lambda must be rejected")

    unreachable_nested_mint = safe_fallback.replace(
        "        GpuMintResult mint = mintGpuOrDegrade(surface, gpuRhi, meta, renderFence, cpuFallback);",
        "        return 0;\n"
        "        if (true) {\n"
        "            GpuMintResult mint = mintGpuOrDegrade(surface, gpuRhi, meta, renderFence, "
        "cpuFallback);\n"
        "        }",
    )
    try:
        audit_apple_readback_fallback(unreachable_nested_mint,
                                      "unreachable nested Apple mint mutation")
    except AssertionError:
        pass
    else:
        raise AssertionError("nested Apple mint after an ancestor return must be rejected")

    nested_uninvoked_mint = safe_fallback.replace(
        "        GpuMintResult mint = mintGpuOrDegrade(surface, gpuRhi, meta, renderFence, cpuFallback);",
        "        auto neverCalled = [&] {\n"
        "            GpuMintResult mint = mintGpuOrDegrade(surface, gpuRhi, meta, renderFence, "
        "cpuFallback);\n"
        "        };",
    )
    try:
        audit_apple_readback_fallback(nested_uninvoked_mint,
                                      "nested uninvoked Apple mint mutation")
    except AssertionError:
        pass
    else:
        raise AssertionError("Apple mint hidden in an uninvoked lambda must be rejected")

    dead_dummy_then_unsafe_mint = safe_fallback.replace(
        "        GpuMintResult mint = mintGpuOrDegrade(surface, gpuRhi, meta, renderFence, cpuFallback);",
        "        if (false)\n"
        "            GpuMintResult mint = mintGpuOrDegrade(surface, gpuRhi, meta, renderFence, cpuFallback);\n"
        "        GpuMintResult realMint = mintGpuOrDegrade(surface, gpuRhi, meta, renderFence,\n"
        "            [] { return CpuPlanes{}; });",
    )
    try:
        audit_apple_readback_fallback(dead_dummy_then_unsafe_mint,
                                      "dead dummy Apple mint mutation")
    except AssertionError:
        pass
    else:
        raise AssertionError("dead dummy mint plus unsafe real mint must be rejected")

    for dead_mint, message in (
        (safe_fallback.replace(
            "        GpuMintResult mint = mintGpuOrDegrade(surface, gpuRhi, meta, renderFence, cpuFallback);",
            "        while (false) {\n"
            "            GpuMintResult mint = mintGpuOrDegrade(surface, gpuRhi, meta, renderFence, cpuFallback);\n"
            "        }",
        ), "mint inside while(false) must be rejected"),
        (safe_fallback.replace(
            "        GpuMintResult mint = mintGpuOrDegrade(surface, gpuRhi, meta, renderFence, cpuFallback);",
            "        if constexpr (false) {\n"
            "            GpuMintResult mint = mintGpuOrDegrade(surface, gpuRhi, meta, renderFence, cpuFallback);\n"
            "        }",
        ), "mint inside if constexpr(false) must be rejected"),
        (safe_fallback.replace(
            "        GpuMintResult mint = mintGpuOrDegrade(surface, gpuRhi, meta, renderFence, cpuFallback);",
            "        return 0;\n"
            "        GpuMintResult mint = mintGpuOrDegrade(surface, gpuRhi, meta, renderFence, cpuFallback);",
        ), "mint after an unconditional return must be rejected"),
    ):
        try:
            audit_apple_readback_fallback(dead_mint, "unreachable Apple mint mutation")
        except AssertionError:
            continue
        raise AssertionError(message)


def main():
    if len(sys.argv) != 16:
        raise SystemExit(
            "usage: test_gpu_scope_completion_static.py "
            "<nativevideoencoder_videotoolbox.mm> "
            "<nativevideoencoder_mediafoundation.cpp> "
            "<applegpusurface_apple.mm> <wingpuimportedge.cpp> "
            "<gpuframedata.cpp> <gpusurfaceallocator.cpp> "
            "<vtkeepsurfaceimporter_apple.mm> <gpucompositor.cpp> "
            "<asyncgpureadbacksink.cpp> <decklinksink.cpp> <outputbusengine.cpp> "
            "<gpuframeretirequeue.cpp> <gpuencodepump.cpp> <gpufence_apple.mm> "
            "<playbackworker.cpp>"
        )

    mutation_self_tests()
    exact_synchronization_mutation_self_tests()
    apple_fence_factory_mutation_self_tests()
    bound_apple_surface_domain_mutation_self_tests()
    apple_rhi_surface_compatibility_mutation_self_tests()
    apple_compositor_context_binding_mutation_self_tests()
    apple_bound_wrap_calls_mutation_self_tests()
    gpu_read_lease_snapshot_model_mutation_self_tests()
    imported_nv12_backing_lifetime_mutation_self_tests()
    apple_scoped_wrapper_pixel_format_mutation_self_tests()
    terminal_import_audit_mutation_self_tests()
    scoped_surface_escape_mutation_self_tests()
    native_submit_adapter_mutation_self_tests()
    first_party_source_discovery_mutation_self_tests()
    fixed_pending_fence_mutation_self_tests()
    exact_retained_owner_mutation_self_tests()
    scoped_readback_boundary_mutation_self_tests()
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
    playback_worker = Path(sys.argv[15]).read_text(encoding="utf-8")
    frame_data_path = Path(sys.argv[5])

    audit_apple_fence_factory(apple_fence, "Apple default Metal fence factory")
    audit_apple_finite_fence_wait(apple_fence, "Apple Metal fence finite wait")
    audit_imported_nv12_backing_lifetime(compositor, "GPU compositor imported NV12 lifetime")
    audit_bound_apple_surface_domain(apple_surface, "Apple surface compatibility")
    apple_rhi = (frame_data_path.parents[2] / "playback/gpu/gpurhicontext_apple.mm").read_text(
        encoding="utf-8")
    audit_apple_rhi_surface_compatibility(apple_rhi, "Apple RHI surface compatibility")
    audit_apple_rhi_readback_cleanup(apple_rhi, "Apple QRhi readback cleanup")
    audit_apple_timed_device_loss_poll(apple_rhi, "Apple timed device-loss polling")
    windows_rhi = (frame_data_path.parents[2] / "playback/gpu/gpurhicontext_win.cpp").read_text(
        encoding="utf-8")
    audit_apple_timed_device_loss_poll(windows_rhi, "Windows timed device-loss polling")
    stub_rhi = (frame_data_path.parents[2] / "playback/gpu/gpurhicontext_stub.cpp").read_text(
        encoding="utf-8")
    audit_stub_synchronous_invoke_fail_stop(stub_rhi, "Stub synchronous render invoke")
    compositor_apple = (frame_data_path.parents[2] / "playback/gpu/gpucompositor_apple.mm").read_text(
        encoding="utf-8")
    audit_apple_compositor_context_binding(compositor_apple,
                                           "Apple compositor context binding")
    audit_compositor_frame_disarm(compositor, "GPU compositor frame cleanup")
    audit_terminal_wait_contract(playback_worker, "terminal GPU owner teardown")
    audit_win_import_device_loss_sticky(win_import, "Windows GPU import device loss")
    audit_terminal_quarantine_participant_promotion(playback_worker,
                                                    "terminal GPU owner quarantine")
    audit_retire_queue_exception_safety(retire_queue, playback_worker,
                                        "GPU retire queue exception safety")
    audit_terminal_import_reaping(playback_worker, win_import,
                                  "terminal Windows import reaping")
    audit_apple_bound_wrap_calls(vt_importer, playback_worker, "Apple decoded surface binding")
    lease_header = (frame_data_path.parents[2] / "playback/gpu/gpusurfacelease.h").read_text(
        encoding="utf-8")
    audit_gpu_read_lease_snapshot_model(lease_header, "GPU read lease snapshot ownership")
    audit_apple_scoped_wrapper_pixel_format(
        apple_surface, "Apple scoped pixel-buffer wrapper format")
    audit_scoped_readback_helper(frame_data, "GpuFrameData scoped readback helper")
    audit_apple_readback_fallback(playback_worker, "PlaybackWorker Apple CPU fallback")
    audit_production_raw_readback_calls(frame_data_path.parents[2], frame_data_path)
    audit_pending_fence_production(frame_data_path.parents[2])
    audit_exact_retained_owner_production(frame_data_path.parents[2], frame_data, compositor,
                                          apple_surface)

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
        function_block(decklink, "NativeGpuProducerStatus waitForNativeGpuProducer"),
        "DeckLink producer wait")
    audit_exact_synchronization_flow(
        function_block(output_bus, "OutputBusFrame OutputBusEngine::renderSingleSource"),
        "output bus reuse", require_pair_fields=False)
    audit_exact_synchronization_flow(
        function_block(retire_queue, "void GpuFrameRetireQueue::collect"),
        "frame retirement queue")

    pump_submit = function_block(encode_pump, "bool GpuEncodePump::submit")
    audit_exact_synchronization_flow(
        pump_submit, "GPU encode pump submit", require_pair_fields=False)
    require(re.search(r"Job\s*&\s*job\s*=\s*m_jobs\s*\[\s*index\s*\]\s*;", pump_submit),
            "GPU encode pump must select a bounded Job slot for the submitted frame")
    require(re.search(r"job\s*\.\s*synchronization\s*=\s*synchronization\s*;", pump_submit),
            "GPU encode pump must store the frame's exact synchronization in that job slot")
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
