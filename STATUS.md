# KairoRenderer v1 Status

**Frozen v1 source completion: 95%.**

- Wave: `C`
- Frozen scope: `realtime-multibackend-v1`
- Source gate: `complete`
- Exact-head execution: `pending_external_runner`
- Verification gate: `cmake-build-ctest-native-smoke`
- Research track: `R1,R6`
- Warning policy: `zero-kairo-owned-warnings`

The 95% score measures the bounded v1 implementation, integration contract,
tests/diagnostics surface and documentation. Native/platform execution evidence
is tracked separately and is never inferred from this score.

## Explicitly post-v1

- full render-graph native barrier migration
- hardware timestamp telemetry
- advanced shadow atlas

See `STATUS.yaml` for the machine-readable contract.
