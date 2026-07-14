# Formal / model-checking gate for the playback transport.
#
# Runs the exhaustive (bounded) explicit-state model checker for the play-epoch
# re-anchor protocol in `all` mode. It exits non-zero unless fixed (F1 + every
# F2 commit family) PROVES the invariant while mut_f1 and every owner-specific
# mut_f2_<family> REFUTE it with a concrete counterexample. This differential
# contract is a gate a vacuous model cannot pass. So any future edit to the
# re-anchor protocol that breaks the proof turns this test RED, replacing the
# manual concurrency-review obligation with a machine gate.
# State totals are aggregates across eleven independently explored scenario
# graphs; they do not describe one coupled 523-state graph.
#
# The checker is pure stdlib Python (milliseconds), so it carries the fast unit/ci
# labels. It lives beside the design writeup it substantiates
# (docs/hardest-technical-challenges.md); this only wires it into CTest.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(NAME transport_epoch_modelcheck
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/docs/hardest-technical-challenges/transport_epoch_modelcheck.py" all)
set_tests_properties(transport_epoch_modelcheck PROPERTIES LABELS "formal;unit;ci" TIMEOUT 120)

add_test(NAME transport_epoch_source_audit
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/transport_epoch_source_audit.py" "${CMAKE_SOURCE_DIR}")
set_tests_properties(transport_epoch_source_audit PROPERTIES LABELS "formal;unit;ci" TIMEOUT 30)
