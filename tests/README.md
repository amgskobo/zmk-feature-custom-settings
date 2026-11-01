# Owner tests

This repository owns its executable test logic and fixtures under `tests/`.

The Renode wired-split fixture resolves `zmk-studio-messages` through the actual
west project selection, supporting parent and isolated workspaces. It refuses
failed, ambiguous or incomplete selections instead of choosing a stale schema
from a recursive directory search. `test_renode_proto_paths.py` retains nine
selection/error/recovery controls and is included in the canonical owner entry.
These controls do not replace the complete Renode smoke and module RPC suite.

## Current test ownership and compatibility

This owner supports DYA custom Studio RPC. Stock ZMK is NOT APPLICABLE;
no feature-disabled stock check is required or counted as compatibility.

Canonical entries (run from the repository root):

```bash
python3 -m unittest discover -v -s tests -p test_owner.py
bash tests/run-web.sh
```

[maintenance.json](maintenance.json) records supported cores, current commands
and explicit gaps. It is a reference map, not a passing test or a claim that
registration/coverage totals establish completeness. Preserve ordered outputs,
raw timestamps and the first failure. Read actual dependencies before execution.

`OWNER_TEST_OUTPUT=/absolute/new/directory` retains owner native/build outputs.
An existing directory fails before tests. Without it a fresh temporary directory
is printed and retained. No shared build directory is deleted. Use
`PYTHONPATH=tests python3 -m unittest -v test_owner.WestCommandsTests.<method>`
for a focused case; discovery runs the complete documented class.

Web assertions and Jest configuration live in `tests/web/`; application source
and installed dependencies remain in `web/`. The package scripts only dispatch
the owner configuration. No dependencies are installed implicitly by the tests.

Guarded production includes in `tests/support/` contain test-only scenarios.
`split_central` also exercises the real relay codec, lock transitions and queued
list/notification retirement. The unchanged unlocked session remains a positive
forwarding control. `async_notification_cases.inc` checks deferred array identity
after genuine view-pool eviction, with a serial control. These are native
event/encoding checks, not physical split or Studio delivery acceptance.
Small production probes remain beside the state they expose. Public sample
settings and diagnostic/fault-injection APIs are production contracts, not
duplicated test algorithms.

The native verifier runs each built binary three times in independent empty
initial-flash directories, retaining raw output before filtering. It checks real
process exits, fatal assertions and every linked checked-init result before
comparing unchanged owner snapshots. A SYS_INIT error cannot be hidden by a
later matching key trace. Native scheduling and controlled APIs still do not
establish physical transport/retention. Test-only core/tool dependencies are
pinned; update pins only after reviewing the new sources and rerunning this
owner matrix. No network updates are implicit in the owner test command.

## Reproducible isolated checks

Use this owner's `west/west-test-isolated.yml` and its pinned test dependency
manifest. Test ZMK is `e5c9b6915b56801193e359dd9bad4a167ce0d1b8`;
Zephyr is `10ba6d0cb38bc3d258775d27982f707599320085`. Resolve the
manifest and verify actual checkout identities before running. Do not inherit
global CMake module overrides or moving production dependency references into
these isolated fixtures. The CI SDK image is pinned by digest in
`.github/workflows/zmk-module.yml`; production dependency manifests are separate.

Use a new `OWNER_TEST_OUTPUT` directory for every complete owner run. Native
repeats reuse one compiled binary with independent initial-flash state; keep
the first failure and raw combined stdout/stderr order. DYA-only compatibility
still means no stock-ZMK test requirement.

The test/tool static CI job installs pre-commit 4.7.0 and uses the pinned
Ruff/clang-format/formatting hooks. Its SKIP list avoids repeating the firmware
and Web jobs. Local full pre-commit execution may run those functional hooks;
prepare Web dependencies with the normal package lifecycle first where present.
A successful hook exit must also leave source inputs unchanged. Snapshot and
event-pattern bytes are excluded from whitespace rewriting.

The existing checked native `custom_settings_public_suppress_test_init` also
asserts call-scoped quiet scalar, small/large raw, keyspace typed/raw and save
operations. A real event observer verifies one core event and its explicit flag;
the real Studio listener must submit zero quiet notifications and exactly one
ordinary notification. A nested ordinary writer inside a quiet core event must
still notify. Its preexisting post-bracket count is captured before the added
cases and remains the value observed by the unchanged snapshot. This proves
event/queue-submission selection, not physical delivery or congestion recovery.

For the identified quiet-guard negative control, invoke
`python3 tests/quiet_notify_negative_control.py /absolute/isolated-owner/src/studio/custom_settings_handler.c`
from the original checkout. The operator refuses its own checkout. Run the same
focused native owner command from that isolated copy, in a fresh output path;
it must fail the checked public-suppression fixture. No dependency is patched,
no assertion is removed and no snapshot is accepted. Exact quiet scalar and
small/large/keyspace payload reads are asserted as well as events and counts.

The real-kernel [settings boundary fixture](settings-boundary/README.md) also
checks stored keyspace ordinal parsing through the production settings load
handler. Decimal names must fit uint32 and the keyspace capacity before their
payload is read. Invalid initial and later records cannot create a wrapped
slot or alter valid entries and independent scalar settings; leading zeros
remain accepted for valid slot numbers. The features binary runs forty-nine
scenarios, with the same binary repeated three times in fresh processes.
`ordinal_overflow_negative_control.py` restores only unchecked accumulation
in an isolated source copy; the unchanged real-loader assertion must fail.
This controlled backend verifies parsing/state behavior, not physical NVS
corruption, power loss or storage retention.

The same fixture covers small/large keyspace rename failure stages. Validation
and allocation refusal preserve RAM and durable entries; a backend PERSIST
pre-write refusal retains the renamed RAM entry as dirty without a changed event. Save
retry and discard/reload recover through the real settings APIs, preserving
the same slot, exact payload and independent entries. This is the existing
RAM-first persistence contract, not transactional rename or physical NVS
failure acceptance. The old durable key is retained by the controlled pre-write
refusal; arbitrary physical storage errors provide no atomic durability guarantee.

The `array-ordinal` case separately checks the production array-name parser:
overflowing numbers and overlong names with hidden non-decimal suffixes are
refused before payload reads, preserving valid elements, dirty state, length
and independent settings. `array_ordinal_negative_control.py` removes each
guard separately in an isolated src copy; the unchanged real-loader checks
must fail three times per fault. First/last valid indices and leading zeros
remain supported.

The owner command boundary limits `zmk-build` to one target at a time by
default. `OWNER_TEST_BUILD_PARALLELISM` selects an explicit positive target
count; account for compiler jobs and concurrent owners in the global budget.
`tests/test_build_budget.py` checks default/explicit forwarding and rejects
invalid budgets before launching west. The canonical owner entry includes
these controls without dropping any native or board scenario.

Native result admission requires every `native_sim.keymap` fixture to have a
matching binary. `required_hooks.json` names the expected checked initialization
hooks and required feature/role configuration. Fixture-bound declarations also
reject missing configuration when both the request and build omit it; excluded
roles cannot replace the requested role. The verifier retains all three raw
runs before comparing unchanged ordered snapshots and rejects logged failures,
fatal output and missing/nonzero completions. `tests/test_native_verifier.py`
owns eleven real-ELF admission controls, including a deliberately missing
native scenario, wrong-role and unrelated-fixture controls.

`bash tests/run-web.sh` is the canonical schema-generation/Web test entry.
It runs all configured Jest scenarios in one worker process. The pre-commit
Jest hook dispatches this same owner entry so nested workers do not multiply
the global container budget. Build and lint retain their separate package entries.

The additional `mutation-*` boundary scenarios cover serialized push/pop and
mixed tail operations, duplicate create/rename, delete/reuse, settings-lock
ordering, invalid structural modes and ordinal persistence flags after shifts.
Scheduling wrappers only pause before lock acquisition or after unlock; all
mutations use unchanged public APIs and real kernel mutexes/threads. A serial
positive control and unchanged independent settings remain required. The same
binary runs each scenario three times in fresh processes. The original code
fails the fifteen regression scenarios; the positive control passes.

`rpc_write_mode_cases.inc` extends the existing checked request-decode hook with
actual encode/decode/dispatch checks for unknown mode values on all five write
arms. The peripheral fixture checks the actual relay codec and unchanged state.
No wire IDs, valid modes, existing hooks or snapshots are replaced.

`sessionLifecycle.spec.tsx` uses the real provider and deferred transport replies
to check connection/index/catalog retirement, no later import/chunk request,
immediate list disposal, a bounded continuously active list, an unrelated
provider update and StrictMode cleanup replay. It does not establish physical
serial/BLE delivery or cancellation of a request already sent to a keyboard.

`mutation-load-view` pauses a real backend record read on a sysworkq handoff
that churns more views than the bounded pool. The loader must apply the exact
record to its original descriptor/index and preserve the independent array.
`chunk_identity_cases.inc` deletes/recreates a slot between real handler chunks;
the stale commit must return `-EAGAIN` and preserve the replacement payload.
Staging, target resolution and commit share the registry guard. Chunk frames
must keep their reference, total size and mode; any keyspace lifecycle/reload
invalidates a pending keyspace transfer, which must restart at offset zero.

The checked core init also runs `behavior_load_cases.inc` in the local-ID
configuration. It rejects unresolved runtime writes, loads range-valid stored
behavior/ID values before lookup resolution and cleans up independent records.
The completion marker is outside existing event snapshots. This preserves the
contract proposed by upstream PR #58 without replacing any original assertion.

`notification_cases.inc` adds seven boundary scenarios: a serial control,
unlock-to-dispatch view churn, nested listener churn, temporary values, temporary
rollback, persistence beyond the view-pool size, and full retained-pool refusal
with recovery. The race fails three times before retention while the serial
control passes. The final features binary runs forty-nine scenarios, three
times each. The controlled backend has room for the long-array records; its
failure injection, prior cases and assertions remain intact. The fixture also
verifies lock-free listeners, exact values/identities, independent settings and
post-dispatch recycling. This is not physical delivery or retention evidence.

`python3 tests/notification_view_negative_control.py ISOLATED_ARRAY_SOURCE`
removes only the retained-view recycling guard in a separate source copy. Run
the unchanged settings boundary entry with `CUSTOM_SETTINGS_SOURCE_DIR` selecting
that copy and fresh evidence. The event-identity and retained-pool assertions
must fail; the serial control and earlier boundary scenarios must still pass.
The operator refuses to modify its owning checkout.

`mutation-load-size` extends the real-backend handoff to the array length
record. Empty and shortened scope save/discard/reset cases assert exact values,
length, affected counts, dirty flags, independent state and reload behavior.
Whole-array reset must remove inactive records too. All six added boundary
cases run three times in the features binary; the prior scenarios remain.

`scope_permission_cases.inc` runs within the existing checked request-decode
hook. Actual encode/decode/dispatch of save/discard/reset must refuse locked
secure keyspace targets by public key, storage ordinal and intersecting prefix.
It also checks the protected empty namespace and unrelated/unlocked controls,
restores Studio lock state and removes its synthetic entry. This exercises the
real Studio lock API and registry/backend; physical transport remains separate.


## Deferred Studio notification regressions

`support/async_notification_cases.inc` raises a real array-element change, then
retains enough synthetic views to force the borrowed target address to be
reused. It checks that reuse happened before releasing the pool and draining
the actual notification worker. The resulting key/index stays original; serial
delivery remains a positive control. Restoring a borrowed pointer in the queue
must fail this case. The checked init is required in Studio and split central.

`support/relay_security_cases.inc` uses actual lock events, work handlers and
nanopb payloads. Locked receive, queued-then-locked and queued-then-lock/unlock
notifications are dropped. Delayed lists retire with that session too; an
unchanged unlocked session still forwards. This checks dispatch authorization,
not cancellation of bytes already sent over physical transport.

The Web RPC fixture checks independent split sources for scalar/array JSON,
prototype-named public keys, and large local BYTES/UTF-8 STRING imports. Only
local large values use the chunk path; split chunk writes remain unsupported.
