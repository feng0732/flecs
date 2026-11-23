# Snapshots

The snapshot addon makes it possible to capture the complete state of a world
and restore it later. This is useful for deterministic rollback, most notably
[rollback netcode](#rollback-netcode), where the result of running the world
after a restore must be identical to running it the first time.

A snapshot is an opaque `ecs_snapshot_t*` handle created with
`ecs_snapshot_take()`, restored with `ecs_snapshot_restore()` and freed with
`ecs_snapshot_free()`.

```c
ecs_snapshot_t *snap = ecs_snapshot_take(world);

// ... mutate the world ...

if (ecs_snapshot_restore(world, snap) == EcsSnapshotOk) {
    // world is back to the state it had when the snapshot was taken
}

ecs_snapshot_free(snap);
```

## What is captured

A snapshot captures everything that is needed to reproduce the world exactly:

- **Entity liveness and generations.** Which entity ids are alive, their
  generation, the order in which ids were issued and the last issued id. After a
  restore, new entities are handed out the same ids as they were during the
  original run.
- **The hierarchy.** Both the fragmenting `(ChildOf, parent)` relationship and
  the non-fragmenting `EcsParent` component.
- **Component data.** For every owned component, its value (regular and sparse
  storage alike), and whether a component is enabled/disabled (toggle state).
- **Version-relevant state.** Table versions and the entity id index are
  restored so that cached references (`ecs_ref_t`) and id generation behave the
  same way as they did in the original run.

The following are intentionally *not* captured, because they describe the
structure of the world (schema) rather than its simulation state, and must stay
intact for restore to keep working:

- Component and relationship registration entities (anything with
  `EcsComponent`), including meta types and modules.
- Builtin entities and the `flecs` module tree.
- Poly objects: observers, queries and systems.

Because poly entities are never captured or removed, observers, queries and
systems that were registered before the snapshot was taken keep working after a
restore.

## Diff-based restore

Restore is a *diff*: only entities and components that differ between the
snapshot and the current world are changed. Consequences:

- Restoring a snapshot that matches the current state returns
  `EcsSnapshotNoChanges` and does not invoke a single lifecycle hook or
  observer.
- Restoring the same snapshot twice is safe; the second restore is a no-op.
- Any user entity that was created after the snapshot was taken is removed.
- Any user component that was added, removed or changed after the snapshot is
  reverted.

This means a restore never triggers spurious `OnAdd`/`OnRemove`/`OnSet` events
for state that did not change, which is important for applications that react to
those events.

## Result codes

Snapshot operations return `ecs_snapshot_result_t` so that failures are
detectable instead of silent:

| Result | Meaning |
|---|---|
| `EcsSnapshotOk` | Restore succeeded and the world was changed. |
| `EcsSnapshotNoChanges` | The world already matched the snapshot; nothing changed and no events were emitted. |
| `EcsSnapshotInvalidSnapshot` | The snapshot is `NULL`, was freed, or is not a valid snapshot. |
| `EcsSnapshotInvalidWorld` | The world is invalid or in readonly/multithreaded/fini mode. |
| `EcsSnapshotInvalidOperation` | Snapshot taken/restored while commands are being flushed, or with pending commands. |
| `EcsSnapshotEmpty` | The snapshot contained no user entities; the world was restored to an empty state. |
| `EcsSnapshotInvalidHandle` | The snapshot references an entity id that is no longer compatible with the world (a dangling pair target). |
| `EcsSnapshotVersionMismatch` | The snapshot was created with an incompatible version, or refers to component ids that don't exist in the world. |

`ecs_snapshot_result_str()` converts a result code to a string, which is handy
for logging.

## Validation before mutation

Before a restore changes anything, the snapshot is validated against the world.
If validation fails the world is left untouched and the corresponding result
code is returned. This guarantees that a failed restore never leaves the world
in a half-updated state.

`ecs_snapshot_take()` also refuses to create a snapshot when the world is in
readonly/multithreaded mode, during fini, while commands are being flushed, or
when there are still pending (deferred) commands. Merge commands first (for
example by ending the frame) before taking a snapshot.

## Detecting freed snapshots

A snapshot handle stays allocated until `ecs_snapshot_free()` is called, so
dangling handles can be detected rather than crashing:

```c
ecs_snapshot_free(snap);
assert(!ecs_snapshot_is_valid(snap));          // true
assert(ecs_snapshot_restore(world, snap) == EcsSnapshotInvalidSnapshot);
```

## Introspection

- `ecs_snapshot_is_valid()` tests whether a handle is live.
- `ecs_snapshot_id()` returns a unique id for the snapshot.
- `ecs_snapshot_entity_count()` returns the number of captured entities.
- `ecs_snapshot_table_count()` returns the number of captured tables.

## Rollback netcode

A typical rollback loop captures a snapshot at a known-good frame, predicts
forward, and rewinds when the prediction turns out to be wrong:

```c
ecs_snapshot_t *confirmed = ecs_snapshot_take(world);

// Predict a few frames using local input.
for (int i = 0; i < N; i ++) {
    apply_local_input(world, frame + i);
    ecs_progress(world, delta_time);
}

// Remote input arrived and differs from the prediction: rewind and replay.
if (input_mismatch) {
    ecs_snapshot_restore(world, confirmed);
    for (int i = 0; i < N; i ++) {
        apply_correct_input(world, frame + i);
        ecs_progress(world, delta_time);
    }
}
```

Because the snapshot restores the entity id index, generations and component
data, replaying the corrected input sequence produces exactly the same state as
a client that had received the correct input from the start.

## Addon configuration

The snapshot addon is enabled by default. To exclude it from a custom build,
either don't select it in a `FLECS_CUSTOM_BUILD` configuration, or define
`FLECS_NO_SNAPSHOT`.
