# Settings kernel boundaries

Run from this module's resolved west workspace:

```bash
bash tests/settings-boundary/run.sh
```

The owner `python3 -m unittest test` command includes this check. Two small
native_sim binaries compile the production sources, real public headers,
Zephyr settings/mutex/thread code and ZMK's event manager. The core variant
uses two int32 settings. The features variant also compiles array, pool,
keyspace and converter code and adds their persistence failure/retry checks.
The backend is controlled, not physical NVS. Link wrappers signal entry into
the real settings APIs; they neither replace the settings mutex nor change
production source. Production features/config are unchanged by this fixture.

Each scenario runs in a fresh process three times. The core variant has eight
scenarios; the features variant has those eight plus three data-feature cases:

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

Set `CUSTOM_SETTINGS_NATIVE_ARTIFACTS` to retain config, binary, build log,
ordered raw per-run logs and exit statuses. An existing status file is never
overwritten. The command returns nonzero if any contract fails. Do not change
assertions merely to make it green. `CUSTOM_SETTINGS_SOURCE_DIR` can select an
isolated production-source copy for a negative control without modifying the
active checkout.

The production lock guard acquires Zephyr's internal `settings_lock` before the
module state lock, including recursive reloads. This selected Zephyr revision
exports that symbol; it is an internal dependency that must be checked on
upgrades. Reads serialize with settings I/O. The test proves ordering and error
state behavior, not reduced latency or nonblocking I/O. Scoped operations and
array reloads retain successful partial work; they are not transactions.

This checks the selected DYA/native_sim workspace. It does not establish older
upstream native_posix compatibility, radio/Studio delivery, physical NVS,
reset/power behavior or XIAO hardware acceptance.
