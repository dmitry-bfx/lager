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
after the root-record body write, and after the publication marker is durable.
The expected recovery is the previous root for the first three boundaries and
the new root after publication.

On POSIX, the shared-ephemeral check also attaches a second process to one named
arena, observes its root update through the mapped atomic root table, rejects a
stale writer, and rejects a writer whose exact cross-substore dependency
changed.

Persistent root journals also contain a fixed 32-store mapped atomic root
cache. Attached runtimes use it for lock-free interprocess root reads; the
append-only journal remains the durable source of truth and repairs cache state
when a process opens the journal after an interrupted publication.

## Reader and GC boundary

The integration test holds the transaction's old snapshot across an update and
checks that the new vector still shares Immer nodes with it. This confirms
structural sharing while both snapshots are alive; it does not validate
reclamation safety.

The durable root journal and mapped node bytes are process-shared. Runtime
domain descriptors, `shared_ptr` snapshot pins, extent-directory snapshots,
and slot bitmaps are process-local. The current collector requires callers to
list roots explicitly; it does not discover each runtime's current roots,
transaction candidates, or snapshots in other processes. Mapped extents set
`can_reclaim_slots` to false, so collection does not make slots reusable. There
are no cross-process reader slots or epoch protocol yet, and no collector runs
concurrently with the integration readers. Concurrent multiprocess GC and
reader-safe slot reuse remain unsupported.
