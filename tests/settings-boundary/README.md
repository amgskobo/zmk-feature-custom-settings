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
scenarios; the features variant has those ten plus six data-feature cases:

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
