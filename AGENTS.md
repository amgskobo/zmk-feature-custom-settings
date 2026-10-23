This repository contains a ZMK module with Web UI using the **unofficial** custom ZMK Studio RPC protocol.

## Dev Rules

- Keep changes local and uncommitted unless the user explicitly requests publication. Run applicable pre-commit checks without staging unrelated work.
- Write simple and sufficient tests for new features.
  - Unit test: Test major functionalities as much as possible by adding case to `tests/<test case>`.
    You might have to add test only logic like executing logic at zephyr initialization to improve test coverage.
  - Build test: Enable feature in `tests/zmk-config/*`. It is to verify build works for real device and to easily test with real device.
    Ensure the feature and device is enabled as expected in the build by verifying output in `tests/test_owner.py`.
- Update README.md properly to guide how to use the module to unfamiliar ZMK keyboard users. Keep the guide simple but sufficient!
- Do not open or update a pull request unless the user explicitly requests it.
- When implementing new features, follow this order: proto definition → firmware handler → web UI

### Tips

- **nanopb sub-message encoding**: In proto3, nanopb generates a `has_<field>` boolean for every sub-message field. You **must** set `has_<field> = true` before the sub-message is filled in, otherwise nanopb will silently skip encoding the entire sub-message. Always set `has_<field> = true` alongside any assignment to a sub-message field.
- **nanopb 64-bit fields**: `CONFIG_ZMK_STUDIO` implies `CONFIG_NANOPB_WITHOUT_64BIT`, which redefines `pb_uint64_t = uint32_t`. Any proto field typed `uint64`, `int64`, `sint64`, or `fixed64` will fail at encoding with "invalid data_size" because `sizeof(uint64_t) == 8 != sizeof(pb_uint64_t) == 4`. **Never use 64-bit proto field types in this module.** Use `uint32` / `int32` instead (e.g. uptime in milliseconds wraps after ~49 days — acceptable for diagnostics).

## Commands

Test command usually takes 1min.

```
# Run lint and test when required
pre-commit run
# Run unit test + build test and verify the results
python3 -m unittest discover -s tests -p test_owner.py
# Run build test directly
west zmk-build tests/zmk-config
# Run unit test directly
west zmk-test tests -m .
# Run web tests
cd web && npm test
```

## Canonical owner tests and continuous maintenance

All executable test assertions, fixtures, models, mutation operators, test
selection/configuration and build-result validators live in this repository's
`tests/`. Run the entries documented in `tests/README.md`; no keyboard checkout,
sibling test suite, private backup script or source-rewriting adapter supplies
missing assertions. CI and package scripts may dispatch those entries. Public
sample/diagnostic tools remain production code. Minimal guarded production
probes may expose real state/interleavings, but test scenarios and assertions
belong in `tests/`, not another copy of the production algorithm.

Maintain completeness per owned contract: normal/boundary/invalid behavior,
independent state, ordered outputs/time, reset/cancellation, failure/recovery,
role/feature gates and public protocol/schema where applicable. A registry or
coverage percentage does not establish completeness. Each implementation family
must map to named current fixtures and explicit gaps in the owner contract.
New regressions must fail before the repair or under an identified negative
control. Preserve unique assertions when merging/removing old tests; archive
their source and failure evidence externally first, with a replacement map.

Compatibility follows the individual owner's supported targets. Run both
unchanged stock ZMK upstream and DYA only for a dual-target module. DYA-only
modules require DYA tests, not an artificial stock build or disabled-feature
stock test. Record unsupported targets as NOT APPLICABLE, never PASS. Never
patch a dependency, disable required features or weaken assertions to manufacture
compatibility. Resolve/read the actual supported dependencies and backend.

Keep the current tests small by boundary. Quick runs every behavioral scenario
once with sanitizers where supported. Full retains relevant role/ABI/optimized,
coverage, kernel and supported-core integration checks; it must not drop scenarios
to save time. Use focused changes for daily work and the complete owner matrix
for interface/infrastructure changes and maintenance. Measure redundant build
cost before consolidating it, reuse one binary for identical repeats, use
explicit clean caches/pins, and avoid implicit network updates during testing.

Each run uses a fresh output directory and records commands, source/dependency
identities, feature/role/backend, raw ordered output and failures. An existing
evidence directory must stop the run; never overwrite a previous result. Report
PASS, FAIL, BLOCKED and NOT RUN separately. Browser/radio/physical acceptance is
an explicit gate and is never inferred from host/native/compile success.
Read `tests/maintenance.json` for current coverage/compatibility boundaries.

## No rolling task backups

Do not create or use `C:/zbm-backups`. The user abolished that location.
Keep executable tests in this owner's `tests/`. Use temporary build storage
and fresh meaningful result artifacts; preserve necessary review diffs, source
identities and raw failure logs without rolling whole-tree source backups.
