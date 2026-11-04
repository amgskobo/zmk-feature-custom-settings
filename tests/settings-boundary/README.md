# Settings kernel boundaries

Run from this module's resolved west workspace:

```bash
bash tests/settings-boundary/run.sh
```

The owner `python3 -m unittest discover -v -s tests -p test_owner.py` command includes this check. Two small
native_sim binaries compile the production sources, real public headers,
Zephyr settings/mutex/thread code and ZMK's event manager. The core variant
uses two int32 settings. The features variant also compiles array, pool,
keyspace and converter code and adds their persistence failure/retry checks.
The backend is controlled, not physical NVS. Link wrappers signal entry into
the real settings APIs; they neither replace the settings mutex nor change
production source. Production features/config are unchanged by this fixture.

The fixture disables CMake's west module discovery with `WEST=OFF` and an empty
`ZEPHYR_MODULES`. ZMK sources are supplied explicitly. An empty module list alone
does not prevent Zephyr from loading unrelated workspace modules and their
application Kconfig files into this standalone kernel fixture.

Each scenario runs in a fresh process three times. The core variant has ten
scenarios; the features variant has seventy-seven scenarios, retaining the existing
data, mutation and notification checks and adding these structural cases:

- `mutation-load-size`: churn the bounded view pool during the backend read of
  an array length; the immutable target and independent array must stay exact.
- `mutation-empty-save`, `mutation-empty-discard`, `mutation-empty-reset`:
  operate on a memory-empty array with persisted elements, then reload through
  the real backend. Success must apply to structure, not silently skip it.
- `mutation-short-reset`, `mutation-short-discard`: a shortened nonempty array
  must reset all stored slots or restore all persisted elements, respectively.

The earlier scenarios remain unchanged:

- `control`: serialized save/discard/reset, same-thread recursive reload and
  independent state.
- `scope`: exact first error, successful count, retained partial changes and retry.
- `save`, `discard`, `reset`: a loader gated inside the real settings lock while
  another thread starts the operation. Two bounded 50 ms joins detect failure
  to complete. Semaphore gates establish the interleaving; host load does not
  excuse a failure. A failing process exits without aborting a mutex owner.
- `discard-error`, `persist-error`, `missing`: read/write/missing-record failures
  preserve visible value, dirty state and notification behavior, followed by retry.
- `array-error`, `blob-error`, `keyspace-error`: failed persistence and reload
  retain recoverable state; successful retry restores exact values/array length.
- `keyspace-ordinal`: the real settings loader rejects empty, non-decimal,
  out-of-capacity, uint32-overflow and very long stored ordinal names before
  reading their payload. An invalid first record cannot claim slot zero;
  subsequent invalid records cannot change valid slots zero/one, their keys,
  payloads, dirty state or independent scalar settings. UINT32_MAX and the
  first overflowing number remain distinct boundaries. Leading-zero decimal
  names still select the exact valid slot. Zephyr intentionally ignores a
  handler error during load, so refusal is checked through payload-read counts
  and the real public setting state, rather than the overall load return code.
- `array-ordinal`: the same real load boundary refuses overflowing decimal
  array indices and overlong names before reading payloads. A non-decimal
  suffix beyond the parser's scan bound cannot be ignored. Invalid first and
  later records preserve both valid elements, length, dirty flags and
  independent scalar settings. Valid first/last indices and leading zeros
  still load their exact values.
- `rename-error`, `rename-large-error`: the real small/large write paths keep
  payload and slot identity. Validation and exhausted shared-pool allocation
  refuse without RAM/durable changes or changed events. A backend PERSIST
  refusal before writing instead leaves the new RAM key dirty, with the old durable key and
  no changed event; successful save retry and discard/reload recovery retain
  exact payloads and independent entries/scalars. These assertions describe
  existing RAM-first writes; rename is not a separate transaction. Arbitrary
  physical storage errors provide no atomic durability guarantee.
- `queue-scalar`, `queue-array`, `queue-blob`, `queue-keyspace`: a real settings
  loader holds the storage mutex after applying records, queues a RAM consumer
  before a synchronous work handoff, then waits for the latter. The loader and
  consumer must complete within a bounded 200 ms join and return exact persisted
  values, defaults, sizes, dirty flags and applicable missing/too-small errors.
  This models ZMK's advertising commit handoff; it does not compile Bluetooth
  or assert HCI/radio behavior.
- `memory-guard`: a sysworkq consumer remains blocked while another thread owns
  the module state mutex, then returns exact values after it is released.

Set `CUSTOM_SETTINGS_NATIVE_ARTIFACTS` to retain config, binary, build log,
ordered raw per-run logs and exit statuses. An existing status file is never
overwritten. The command returns nonzero if any contract fails. Do not change
assertions merely to make it green. `CUSTOM_SETTINGS_SOURCE_DIR` can select an
isolated production-source copy for a negative control without modifying the
active checkout.

For the identified ordinal-overflow negative control, copy the owner source
to an isolated directory and run
`python3 tests/ordinal_overflow_negative_control.py /absolute/isolated-owner/src/custom_settings_keyspace.c`
from the original checkout. Select the isolated source with
`CUSTOM_SETTINGS_SOURCE_DIR=/absolute/isolated-owner/src` and rerun this same
fixture in fresh artifacts.
The unchanged `keyspace-ordinal` assertion must fail: `#4294967296` is read
and aliases slot zero without the checked multiply/add. The operator refuses
the active checkout and changes neither assertions nor dependencies.

The array controls use the same fixture and a separate isolated source copy
per fault:
`python3 tests/array_ordinal_negative_control.py /absolute/isolated-owner/src/custom_settings_array.c overflow`
or `truncation`. Select that copy's **src directory** with
`CUSTOM_SETTINGS_SOURCE_DIR`; both controls must produce three failures.
The overflow control aliases `4294967296` to element zero; the truncation
control ignores a non-decimal suffix beyond the scan bound. No assertion or
dependency is changed.

The storage/mutation and borrowed-callback guard acquires Zephyr's internal
`settings_lock` before the module state lock, including recursive reloads.
This selected Zephyr revision exports that symbol; it is an internal dependency
that must be checked on upgrades. Pure RAM lookup/copy operations take only
the state mutex and must not promote it into storage access or invoke arbitrary
callbacks. This avoids a storage-owner/work-queue cycle while preserving writer
ordering and state serialization. The test proves these selected interleavings
and error behavior, not reduced latency, full initialization or nonblocking
I/O. The borrowed-callback API (`zmk_custom_setting_with_value`,
`zmk_custom_setting_with_large_raw_bytes`) keeps these locks for as long as its
callback runs, so a callback must never wait. The Studio value encoder, whose
stream is paced by the host, copies with `zmk_custom_setting_read_into` and
writes with nothing held; the owner's native self-test
`custom_settings_large_chunked_rpc` fails if a settings lock is still owned at
a stream write. Scoped operations and array reloads retain successful partial work; they
are not transactions.

This checks the selected DYA/native_sim workspace. It does not establish older
upstream native_posix compatibility, radio/Studio delivery, physical NVS,
reset/power behavior or XIAO hardware acceptance.

The `mutation-notify-*` scenarios exercise synchronous notification identity
after unlock and during nested lookups, including temporary/rollback values,
long-array persistence, bounded retained-pool exhaustion and recovery. They keep
all earlier scenarios and run in fresh processes with the same binary three
times. `notification_cases.inc` owns the assertions and real-kernel handoff.

## Blob validation and provisional load slots

The additional `review-*` cases preserve all existing scenarios. Two crossed
ordinal-like public names load into their original slots in both record orders.
Malformed terminated-key/prefix/length, payload capacity/type/constraint and
duplicate-key records are refused without claiming a new slot. Caller-owned key
copies retain independent names and reject truncation/deleted slots. Atomic
by-key array writes preserve later elements when growing. `review-scope-save`,
`review-scope-discard` and `review-scope-reset` pause before the real paired guard,
delete/reuse a selected slot on another thread, then check exact replacement
state and a serial matching-target control. Every case uses the same binary
three times in fresh processes. These checks do not establish physical storage
or transport behavior.

The fourteen additional `blob-*` scenarios retain every earlier boundary case.
`blob-capacity` covers small pooled BYTES/STRING exact capacity and oversized
typed/raw/quiet writes in all three modes, retaining temporary and durable
values on refusal. `blob-constraints` compares typed/raw keyspace OPTIONS for
BYTES/STRING, with valid quiet/temporary and unconstrained large controls.
`blob-pool` keeps allocation failure distinct from size failure and preserves
an independent pool member.

`blob-load-{oversize,scratch,read,short,pool}` checks that a rejected stored
record releases its provisional slot and allows a later create. The matching
`blob-load-live-*` cases retain the existing live value, identity and pool links.
`blob-load-valid` is a successful persisted-load/delete/reuse control. All run
three times per features binary against the actual registry, settings loader
and kernel; the backend injects failures. Physical NVS/power loss remains a gap.
