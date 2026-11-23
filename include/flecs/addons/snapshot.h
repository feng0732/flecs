/**
 * @file addons/snapshot.h
 * @brief World state snapshots and restore.
 *
 * The snapshot addon makes it possible to capture the state of a world and
 * later restore it, which is useful for deterministic rollback (for example
 * rollback netcode). Restoring a snapshot brings the world back to the exact
 * state it had when the snapshot was taken, so that continuing to call
 * ecs_progress() from the restored state with the same input produces the same
 * result as it did the first time.
 *
 * A snapshot captures:
 * - which entities are alive, including their generation (so recycled ids are
 *   reproduced exactly);
 * - the hierarchy (ChildOf and the non-fragmenting Parent component);
 * - component data for owned components (regular and sparse);
 * - the enabled/disabled (toggle) state of components;
 * - the entity id index and relevant resource versions (table versions), so
 *   that ids issued after restore and cached references match the original run.
 *
 * Restoring is a *diff* operation: only entities and components that differ
 * from the snapshot are changed. This means that restoring a snapshot that
 * matches the current state does not invoke any lifecycle hooks or observers,
 * and restoring the same snapshot more than once is safe. Observers, queries
 * and systems that were registered before the snapshot was taken keep working
 * after restore.
 */

#ifdef FLECS_SNAPSHOT

#ifndef FLECS_SNAPSHOT_H
#define FLECS_SNAPSHOT_H

/**
 * @defgroup c_addons_snapshot Snapshot
 * @ingroup c_addons
 * World state snapshots and restore.
 *
 * @{
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Result of a snapshot operation.
 * Snapshot operations return a result code so that failures (invalid snapshot,
 * empty snapshot, invalid entity handle, version mismatch, ...) are detectable
 * by the application instead of being silently ignored.
 */
typedef enum ecs_snapshot_result_t {
    /** Operation succeeded, and the world was changed to match the snapshot. */
    EcsSnapshotOk = 0,

    /** The snapshot is valid but the world already matched it. Nothing was
     * changed and no lifecycle hooks or observers were invoked. This makes a
     * repeated restore detectably distinct from the first restore. */
    EcsSnapshotNoChanges,

    /** Generic error, for example a snapshot that could not be created because
     * the world was in readonly/multithreaded mode. */
    EcsSnapshotError,

    /** The snapshot argument is NULL, was already freed, or does not carry a
     * valid snapshot header. */
    EcsSnapshotInvalidSnapshot,

    /** The world argument is invalid, or is in a state that cannot be
     * snapshotted/restored (readonly, multithreaded, pending commands). */
    EcsSnapshotInvalidWorld,

    /** The operation is not allowed at this time (for example a nested
     * snapshot/restore operation, or a restore of a snapshot into the world
     * that is currently progressing). */
    EcsSnapshotInvalidOperation,

    /** The snapshot does not contain any user entities. Restoring it brings the
     * world to an empty state (all user entities are removed); the code is
     * returned to signal that the snapshot carried no entities. */
    EcsSnapshotEmpty,

    /** The snapshot refers to an entity that is currently alive in the world
     * with a different generation. This means the entity was recycled and the
     * snapshot is no longer compatible with the world. */
    EcsSnapshotInvalidHandle,

    /** The snapshot was created with an incompatible build/version, or refers
     * to component ids that no longer exist or have a different generation. */
    EcsSnapshotVersionMismatch
} ecs_snapshot_result_t;

/** Snapshot of world state.
 * Opaque handle returned by ecs_snapshot_take(). Must be freed with
 * ecs_snapshot_free().
 */
typedef struct ecs_snapshot_t ecs_snapshot_t;

/** Create a snapshot of the current world state.
 * The world must not be in readonly or multithreaded mode. Pending commands in
 * stages are flushed before the snapshot is taken so that the snapshot always
 * reflects a consistent state.
 *
 * @param world The world.
 * @return The snapshot, or NULL if the snapshot could not be created. When NULL
 *         is returned an error is logged with ecs_err().
 */
FLECS_API
ecs_snapshot_t* ecs_snapshot_take(
    ecs_world_t *world);

/** Restore the world to the state stored in the snapshot.
 * Only differences between the current world state and the snapshot are
 * applied, so restoring the same snapshot repeatedly is a no-op and does not
 * invoke lifecycle hooks or observers for unchanged state.
 *
 * If the snapshot is discovered to be incompatible with the world (entity
 * handles or component versions don't match) the world is not modified and an
 * error code is returned. All validation is done before any mutation happens.
 *
 * @param world The world.
 * @param snapshot The snapshot to restore.
 * @return A result code.
 */
FLECS_API
ecs_snapshot_result_t ecs_snapshot_restore(
    ecs_world_t *world,
    const ecs_snapshot_t *snapshot);

/** Free a snapshot.
 * It is safe to pass NULL. After freeing, the pointer is invalidated and can be
 * detected by ecs_snapshot_is_valid().
 *
 * @param snapshot The snapshot to free.
 */
FLECS_API
void ecs_snapshot_free(
    ecs_snapshot_t *snapshot);

/** Test whether a snapshot handle is valid (has not been freed).
 *
 * @param snapshot The snapshot.
 * @return True if the handle refers to a live snapshot.
 */
FLECS_API
bool ecs_snapshot_is_valid(
    const ecs_snapshot_t *snapshot);

/** Get the unique id of a snapshot.
 *
 * @param snapshot The snapshot.
 * @return The snapshot id, or 0 if the snapshot is not valid.
 */
FLECS_API
uint64_t ecs_snapshot_id(
    const ecs_snapshot_t *snapshot);

/** Return the number of entities stored in the snapshot.
 * This is the number of (non-builtin) entities captured in the snapshot.
 *
 * @param snapshot The snapshot.
 * @return The number of entities, or -1 if the snapshot is not valid.
 */
FLECS_API
int32_t ecs_snapshot_entity_count(
    const ecs_snapshot_t *snapshot);

/** Return the number of tables stored in the snapshot.
 *
 * @param snapshot The snapshot.
 * @return The number of tables, or -1 if the snapshot is not valid.
 */
FLECS_API
int32_t ecs_snapshot_table_count(
    const ecs_snapshot_t *snapshot);

/** Convert a snapshot result code to a string.
 *
 * @param result The result code.
 * @return String representation of the result code.
 */
FLECS_API
const char* ecs_snapshot_result_str(
    ecs_snapshot_result_t result);

#ifdef __cplusplus
}
#endif

/** @} */

#endif // FLECS_SNAPSHOT_H

#endif // FLECS_SNAPSHOT
