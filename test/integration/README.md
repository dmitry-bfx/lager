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
same root also contains real persistent `immer::map`, `immer::set`,
`immer::table`, and boxed `immer::variant` values; the separate reader checks
all 1,024 entries in each HAMT container and every variant payload element. The
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

The persistent pinned-graft check keeps a second process on an old dependency
snapshot, commits a newer dependency root, then publishes a target root that
reuses the old vector subtree. It captures roots for tracing while the old
reader is still pinned and confirms both the reader and the new root retain the
old value. Slot reclamation remains disabled, so this verifies root capture
and graph reachability, not reuse safety.

`persistent-round-trip-crash-pin-registration` kills a reader immediately after
it claims a persistent pin slot and before it publishes the descriptor. Root
enumeration reaps the dead claimant, after which another reader successfully
uses the pin table. This checks dead-process cleanup, not concurrent mapped
reclamation safety.

`persistent-round-trip-controlled-gc` uses an in-memory, single-process arena
with reclamation enabled. Barriers force a reader to attempt acquisition during
root capture and a writer to publish a pre-capture candidate afterward. The
active transaction's allocation generation protects its candidate while it is
not yet registered as a root. A separate bitmap check confirms allocations
made during marking survive that sweep and become reclaimable in the next
generation. The test keeps an old reader through a graft and a collection,
releases and reuses its pin slot, verifies that a stale release cannot clear
the new pin, then proves that unreachable slots are reclaimed and one is
allocated again. This validates the first controlled arena only; it does not
validate the mapped multiprocess protocol.

The staged synchronization design and the multiprocess protocol still required
before mapped reclamation are described in
[`doc/shared-state-gc-protocol.md`](../../doc/shared-state-gc-protocol.md).

The durable root journal, mapped node bytes, shared-ephemeral root table, and
reader/candidate pin descriptors are process-shared. Runtime domain
descriptors, extent-directory snapshots, and slot bitmaps are process-local.
Each pin table is bounded at 24 simultaneous pins; pins from processes that
have exited and been reaped are cleared when roots are enumerated or a new pin
needs space. Mapped extents still set `can_reclaim_slots` to false, so
collection does not make slots reusable. Allocation-generation coordination,
concurrent multiprocess marking/sweeping, and reader-safe slot reuse remain
unfinished.

## Remaining spec gaps confirmed by this run

The persistent round-trip and transaction tests pass, but the current
generation counters do not yet implement one global sequence across every
shared domain. A persistent root journal sequences the stores recorded in that
journal, a shared-ephemeral arena has its own sequence, and independent
journals have independent sequences. Exact dependency validation is tested
within one journal or one shared-ephemeral arena; cross-kind dependencies and
ordering between those generation namespaces are not established.

Snapshot reads use atomic root/cache loads and do not take the writer commit
lock. Acquiring a process-shared snapshot pin currently allocates a lease and
consumes one of the bounded shared pin slots. That does not meet the spec's
stricter audio-thread target of allocation-free snapshot acquisition. A
lock-free, allocation-free reader registration protocol is still needed before
claiming that requirement.
