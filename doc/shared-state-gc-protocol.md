# Shared storage GC protocol

This document separates the protocol needed to safely reuse slots in one
controlled arena from the later process-shared protocol. The current mapped
persistent and shared-ephemeral backends keep `can_reclaim_slots` disabled.
They do not implement the process-shared protocol below.

## Controlled arena protocol

The first implementation uses one in-process arena coordinator and one arena
allocator mutex. It is a correctness harness, not the final reader protocol.
The coordinator mutex serializes snapshot acquisition, pin-slot updates,
transaction registration, root publication, and root capture. Immutable
traversal after snapshot acquisition takes no coordinator lock. Holding the
coordinator during a local collection is acceptable for this first stage; it
does not meet the final reader or writer latency targets.

The controlled integration harness tests completed candidates registered
before root capture and blocks root changes while capture and sweep run. The
production bitmap marks allocations made while a mark is active, but has no
per-slot allocation-generation metadata. Therefore the generation protocol
below is required work, not a claim about current production behavior. The
harness does not test a candidate allocation racing its first registration.

### Snapshot acquisition and release

1. Lock the coordinator.
2. Load the current root descriptor.
3. Claim a free pin slot, increment its slot sequence, write the root handle and
   generation, then publish the slot as active.
4. Return the snapshot only after the pin is active; unlock the coordinator.
5. On final snapshot release, lock the coordinator, verify the slot sequence
   still matches the snapshot token, clear the root, then make the slot free.

The sequence is a reuse token. A delayed release for sequence `s` cannot clear
a slot that has already been reused with sequence `s+1`. The coordinator makes
the root read and pin publication indivisible relative to root capture. A
reader starting while capture is in progress waits; it then pins the root
current after collection. Reads through an acquired snapshot are ordinary
immutable node loads.

### Writer transactions and allocation generations

1. A transaction registers its base root and every shared read dependency
   before constructing a candidate. Those roots remain registered through
   commit or abort.
2. Arena allocation tags each reserved slot with the current allocation
   generation. A reserved slot is not exposed through a root until its object
   construction completes.
3. Candidate construction publishes the completed candidate handle into the
   transaction record before the transaction may yield or attempt commit. The
   coordinator covers the transaction-record update. The candidate's storage
   construction itself must either be covered by an active allocation
   generation or excluded from concurrent collection.
4. Commit locks the coordinator, checks the base generation and dependencies,
   and atomically replaces the current root descriptor. The transaction record
   remains live until this publication finishes. Abort removes the record;
   its unreachable allocations are eligible at a later collection.

For the complete local protocol, at GC start while holding the coordinator and
allocator mutex, the collector freezes generation `G` and advances the
allocator to `G+1`. Slots allocated in `G+1` or later are excluded from this
sweep. Pre-GC candidate slots belonging to a still-live transaction are
retained, even when the candidate has not yet been published. Once the writer
publishes, the new current root protects the candidate on future collections.
Failed candidates become reclaimable after their transaction record is
removed. Per-slot generation tracking and active-generation exclusion are not
implemented yet.

### Root capture, grafts, and sweep

The collector captures, under the coordinator, the current root of every
domain, every active snapshot pin, every transaction base/read/candidate root,
and explicit persistent snapshots. The complete protocol starts marking
before releasing the allocator mutex, then traces immutable nodes outside the
coordinator. The sweep considers only allocated slots in generations at or
below `G`; it frees an unmarked slot only when it is not owned by a live
transaction generation. Freed slots re-enter the controlled arena's free
bitmap and can be allocated again. The current harness instead holds the
coordinator for the full mark and sweep, which is deliberately more serializing.

A subtree copied from an old snapshot is safe only while its source snapshot
or a transaction read dependency remains pinned until the graft candidate is
registered. The mark traversal of the new root follows the grafted handle and
marks the old subtree. Copying a `storage_ptr` alone is not a lifetime pin.

These rules order the reader/GC race: acquisition before capture publishes a
pin that capture sees; acquisition after capture waits until collection ends
and pins the then-current root. Release and slot reuse are serialized, and the
sequence token prevents a stale release from clearing a new pin. A candidate
built before collection is either in a registered transaction record or the
protocol must not permit collection to begin. Grafts are traced from captured
candidate/current roots as well as their source pins.

## Process-shared extension (not implemented)

Multiprocess reclamation requires shared allocation metadata and a shared
registration handshake; the current process-local bitmaps and mapped root pin
tables are insufficient. The eventual shared slot protocol must distinguish
`FREE`, `CLAIMING`, `ACTIVE`, and `RELEASING` states and tag every claim with a
monotonic slot sequence plus process identity. Process identity must include a
start-time or equivalent reuse-resistant token; a bare PID can refer to an
unrelated process after PID reuse.

* A reader claims a slot before loading the root, writes its descriptor, then
  publishes `ACTIVE` with release ordering and validates the root sequence.
  GC treats `CLAIMING` as a reason to retry/wait, never as an absent root. A
  changed root makes the reader retry before it dereferences any graph node.
* Final release transitions the exact active sequence to `RELEASING`, clears
  its descriptor, and publishes `FREE`. A new reader can claim only `FREE`;
  GC double-reads state and sequence so it cannot combine fields from two
  owners.
* GC advances the shared allocation generation and captures roots at one
  shared synchronization point. Allocation reserves a shared slot with its
  generation before construction. Sweep may free only old-generation,
  unmarked slots after checking that no live transaction owns that generation.
  New-generation allocations are excluded from the current sweep.
* Transactions publish a shared active record before allocating. Their
  pre-capture candidate allocations stay protected by the transaction
  generation until commit/abort. Root publication is serialized per domain;
  persistent publication's durable marker selects either the old root or the
  complete new root after recovery.
* If a process dies in `CLAIMING`, `ACTIVE`, or `RELEASING`, a reaper first
  proves that the exact process instance is dead, then CASes the exact slot
  sequence to a reaping state. It clears the slot only after winning that CAS.
  A dead writer before the durable commit marker leaves an unrooted candidate;
  after the marker, recovery treats the new descriptor as current. Candidate
  pins cannot be dropped before this decision is recoverable.

Tracing and sweeping must not rely on persistent refcounts. Shared allocation
and generation metadata must be reconstructible by tracing durable roots after
crash. This phase requires deterministic multiprocess tests for death during
registration and publication, competing allocation, pin-slot reuse, old-reader
grafts, and actual slot reuse. It must not enable mapped reclamation before
those tests pass.

### Required race review and current evidence

* **Reader enters during root capture:** in the controlled arena, collection
  holds the coordinator from root capture through sweep. The barrier test makes
  a new reader attempt acquisition during that interval; it acquires the
  current root after collection. A reader pinned before capture is included in
  the captured roots. Mapped readers do not share this coordinator today.
* **Pin release followed by slot reuse:** the controlled test releases a pin,
  immediately reuses its slot, then sends the old release token again. The
  incremented sequence leaves the new pin active. The mapped table currently
  serializes reuse by clearing the process ID last, but has no sequence token;
  this needs a process-shared state/sequence protocol before reclamation.
* **Pre-capture candidate published after capture:** the controlled test
  registers the candidate in a live transaction, pauses collection after root
  capture, and attempts publication from another thread. The candidate remains
  live and publication proceeds after sweep. This tests a registered candidate,
  not allocation racing initial transaction registration.
* **Old subtree grafted into a new generation:** the controlled test derives a
  candidate from an old pinned vector, verifies shared node handles, keeps the
  candidate snapshot pinned across collection, and reads the grafted value
  afterward. The mapped pinned-graft process test covers tracing reachability,
  but mapped reclamation is disabled.
* **Death during pin registration or publication:** persistent writer crash
  failpoints cover durable publication boundaries; there is no deterministic
  process-death failpoint during reader pin registration. Dead-reader cleanup
  is exercised by existing process tests, but PID reuse and slot-reuse races
  remain unresolved. No multiprocess reclamation claim is made.

Today, persistent root records, mapped pin descriptors, mapped node bytes, and
the shared-ephemeral root/pin tables are process-shared. `shared_ptr` snapshot
leases, runtime domain descriptors, extent-directory snapshots, and slot
bitmaps are process-local. `roots_for_gc` combines durable current records
with the process-shared reader/candidate pin table under a shared file lock;
root publication uses an exclusive file lock. Reader pin registration itself
publishes the process ID before filling the descriptor and has no matching
collector lock. Snapshot acquisition validates the root after registering and
retries if it changed, but candidate storage is currently constructed before
the commit-time candidate pin is published. Reclamation is disabled for mapped
extents, so that allocation-to-pin gap is not safe for reuse today.

Existing crash failpoints exercise writer death at allocation, data flush,
root-record body write, durable marker, and cache publication. They do not
exercise death during shared pin registration. The process-shared pin reaper
also identifies owners by PID without a PID-reuse-resistant process-start
token. Both are open protocol cases, not covered by the local arena test.

The coordinator lock in the controlled phase is intentionally not the final
audio-thread protocol. Lock-free, allocation-free snapshot registration and
one global generation clock across persistent and ephemeral domains remain
unfinished.
