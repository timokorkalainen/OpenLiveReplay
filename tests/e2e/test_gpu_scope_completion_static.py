#!/usr/bin/env python3
import re
import sys
from pathlib import Path


TOKEN_RE = re.compile(r"[A-Za-z_]\w*|\d+|::|->|==|!=|&&|\|\||[{}()\[\].,;:&*!<>+=/-]")


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


def audit_cached_apple_surface_domain(source, label):
    constructor = function_block(source, "AppleGpuSurface(")
    compatibility = function_block(
        source, "GpuSurfaceCompatibility compatibility() const override")
    compatibility_tokens = [token for token, _ in tokens(compatibility)]
    require("m_deviceDomainId" in compatibility_tokens,
            f"{label}: compatibility must return the cached device domain")
    require("gpuMetalDeviceDomainId" not in compatibility_tokens and
            "m_device" not in compatibility_tokens and "registryID" not in compatibility_tokens,
            f"{label}: compatibility must not query the Objective-C device")
    assignment = re.search(
        r"m_deviceDomainId\s*=\s*gpuMetalDeviceDomainId\s*\(\s*"
        r"(?:\(\s*__bridge\s+void\s*\*\s*\)\s*)?m_device\s*\)",
        constructor,
    )
    require(source.count("gpuMetalDeviceDomainId") == 1 and assignment is not None,
            f"{label}: device domain must be captured exactly once during construction")
    constructor_items = tokens(constructor)
    constructor_braces = token_pairs(constructor_items, "{", "}")
    constructor_body = next(
        (index for index, (token, _) in enumerate(constructor_items) if token == "{"), None)
    assignment_token = next(
        (index for index, (token, position) in enumerate(constructor_items)
         if token == "m_deviceDomainId" and position >= assignment.start()),
        None,
    )
    require(assignment_token is not None and
            direct_enclosing_brace(constructor_braces, assignment_token) == constructor_body and
            not is_obviously_unreachable(constructor_items, assignment_token,
                                          constructor_braces),
            f"{label}: device domain capture must execute directly in construction")


def cached_apple_surface_domain_mutation_self_tests():
    safe = """
class AppleGpuSurface {
public:
    AppleGpuSurface() {
        m_deviceDomainId = gpuMetalDeviceDomainId(m_device);
    }
    GpuSurfaceCompatibility compatibility() const override {
        return {m_deviceDomainId, m_authorityEpoch};
    }
private:
    uintptr_t m_deviceDomainId = 0;
    uint64_t m_authorityEpoch = 0;
};
"""
    audit_cached_apple_surface_domain(safe, "safe cached Apple domain mutation")
    for mutation, message in (
        (safe.replace("m_deviceDomainId, m_authorityEpoch",
                      "gpuMetalDeviceDomainId(device), m_authorityEpoch"),
         "per-query Objective-C domain lookup must be rejected"),
        (safe.replace("m_deviceDomainId = gpuMetalDeviceDomainId(m_device);", ""),
         "missing construction-time domain capture must be rejected"),
        (safe.replace(
            "        m_deviceDomainId = gpuMetalDeviceDomainId(m_device);",
            "        if (false) {\n"
            "            m_deviceDomainId = gpuMetalDeviceDomainId(m_device);\n"
            "        }",
        ), "unreachable construction-time domain capture must be rejected"),
        (safe.replace(
            "        m_deviceDomainId = gpuMetalDeviceDomainId(m_device);\n",
            "",
        ).replace(
            "private:\n",
            "    void refreshDomain() { m_deviceDomainId = gpuMetalDeviceDomainId(m_device); }\n"
            "private:\n",
        ), "domain capture outside construction must be rejected"),
        (safe.replace("gpuMetalDeviceDomainId(m_device)",
                      "gpuMetalDeviceDomainId(otherDevice)"),
         "domain capture from the wrong Metal device must be rejected"),
        (safe.replace("m_deviceDomainId, m_authorityEpoch",
                      "m_deviceDomainId + m_device.registryID, m_authorityEpoch"),
         "compatibility Objective-C device access must be rejected"),
    ):
        try:
            audit_cached_apple_surface_domain(mutation, "cached Apple domain mutation")
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
    source_roots = (
        repo_root / "playback", repo_root / "recorder_engine", repo_root / "project",
        repo_root / "telemetry", repo_root / "midi", repo_root / "streamdeck",
        repo_root / "websocket", repo_root / "ui",
    )
    source_paths = list(repo_root.glob("*.cpp")) + list(repo_root.glob("*.h"))
    for source_root in source_roots:
        if not source_root.exists():
            continue
        for suffix in ("*.cpp", "*.mm", "*.h"):
            source_paths.extend(source_root.rglob(suffix))
    helper_resolved = helper_path.resolve()
    for source_path in source_paths:
        source = source_path.read_text(encoding="utf-8")
        production = production_preprocessor_view(source)
        count = len(RAW_READBACK_CALL_RE.findall(strip_comments_and_literals(production)))
        if source_path.resolve() == helper_resolved:
            require(count == 1, "scoped helper must remain the sole production raw readback caller")
        else:
            require(count == 0,
                    f"direct production raw readback is forbidden: {source_path}")


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
    cached_apple_surface_domain_mutation_self_tests()
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

    audit_apple_fence_factory(apple_fence, "Apple default Metal fence factory")
    audit_cached_apple_surface_domain(apple_surface, "Apple surface compatibility")
    frame_data_path = Path(sys.argv[5])
    audit_scoped_readback_helper(frame_data, "GpuFrameData scoped readback helper")
    audit_apple_readback_fallback(playback_worker, "PlaybackWorker Apple CPU fallback")
    audit_production_raw_readback_calls(frame_data_path.parents[2], frame_data_path)

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
