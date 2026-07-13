# Formal / model-checking gate for the playback transport.
#
# Runs the exhaustive (bounded) explicit-state model checker for the play-epoch
# re-anchor protocol in `all` mode. It exits non-zero unless the unedited protocol
# and all modeled bug shapes (head / mutA / mutB / mutReuse) REFUTE the invariant AND the
# repaired protocol (fixed = F1 + F2) PROVES it over all reachable states — a
# differential gate a vacuous model cannot pass. So any future edit to the
# re-anchor protocol that breaks the proof turns this test RED, replacing the
# manual concurrency-review obligation with a machine gate.
#
# The checker is pure stdlib Python (milliseconds), so it carries the fast unit/ci
# labels. It lives beside the design writeup it substantiates
# (docs/hardest-technical-challenges.md); this only wires it into CTest.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(NAME transport_epoch_modelcheck
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/docs/hardest-technical-challenges/transport_epoch_modelcheck.py" all)
set_tests_properties(transport_epoch_modelcheck PROPERTIES LABELS "formal;unit;ci" TIMEOUT 120)
