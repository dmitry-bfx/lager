# Persistent storage integration checks

The persistent round-trip executable is a separate CMake target so it can be
built without compiling Lager's full unit-test suite. It uses the real Boost
headers (including Boost.Hana) and the real Zug target found by CMake; no
substitute headers are used.

From the workspace superbuild directory, where `immer/` and `lager/` are
siblings, run:

```sh
cmake -S . -B build \
  -Dlager_BUILD_PERSISTENT_ROUND_TRIP=ON \
  -Dlager_BUILD_TESTS=OFF \
  -Dlager_BUILD_EXAMPLES=OFF
cmake --build build --target lager-persistent-round-trip
ctest --test-dir build/lager --output-on-failure \
  -R '^persistent-round-trip-'
```

When Boost or Zug is not installed in a standard CMake package location, add
`-DBoost_DIR=/path/to/BoostConfig.cmake-directory` and
`-DZug_DIR=/path/to/ZugConfig.cmake-directory`. Immer is taken from the
workspace sibling target.

The writer, reader, updater, and final reader are separate process invocations.
The reader reserves the writer's old mapping address before opening the extent,
then validates the reflected layout and indexes every saved vector value. The
crash tests terminate writers after speculative allocation, after data flush,
after the root-record body write, after the durable commit marker but before
cache publication, and after cache publication. An already-attached reader
sees the committed pending descriptor without waiting on the writer, and
journal recovery then reconciles its cache. The expected root is previous
before the commit marker and new at or after it.

On POSIX, the shared-ephemeral check also attaches a second process to one named
arena, observes its root update through the mapped atomic root table, rejects a
stale writer, and rejects a writer whose exact cross-substore dependency
changed.

Persistent root journals also contain a fixed 32-store mapped atomic root
cache and a 24-slot process-shared reader/candidate pin table. Attached
runtimes use the cache for interprocess root reads; each persistent snapshot
and in-flight commit publishes its root in the pin table. The append-only
journal remains the durable source of truth and repairs cache state when a
process opens the journal after an interrupted publication. Attached root
reads remain lock-free during publication by selecting the pending descriptor
after the durable marker; explicit recovery reconciles the mapped cache. A
collector can enumerate current roots and live pins with
`add_persistent_roots`, once for each reflected root layout used by the journal.

Shared-ephemeral arenas also publish roots and a 24-slot process-shared
reader/candidate pin table in their mapping header. `add_shared_ephemeral_roots`
captures current roots and pins under a short shared writer lock.

## Reader and GC boundary

The integration test holds a transaction's old snapshot across an update and
checks that the new vector still shares Immer nodes with it. Separate-process
checks hold old persistent and shared-ephemeral snapshots while another process
commits; the collector sees the old handles in the shared pin tables and the
ephemeral reader's old value stays readable. Reaping that reader removes its
pin. These checks validate pin publication and dead-process cleanup, but not
reclamation safety.

The durable root journal, mapped node bytes, shared-ephemeral root table, and
reader/candidate pin descriptors are process-shared. Runtime domain
descriptors, extent-directory snapshots, and slot bitmaps are process-local.
Each pin table is bounded at 24 simultaneous pins; pins from processes that
have exited and been reaped are cleared when roots are enumerated or a new pin
needs space. Mapped extents still set `can_reclaim_slots` to false, so
collection does not make slots reusable. Allocation-generation coordination,
concurrent multiprocess marking/sweeping, and reader-safe slot reuse remain
unfinished.
