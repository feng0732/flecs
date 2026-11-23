/**
 * @file instantiate.h
 * @brief Functions for instantiating prefabs (IsA relationship).
 */

#ifndef FLECS_INSTANTIATE_H
#define FLECS_INSTANTIATE_H

typedef struct ecs_instantiate_ctx_t {
    ecs_entity_t root_prefab;
    ecs_entity_t root_instance;
} ecs_instantiate_ctx_t;

void flecs_instantiate(
    ecs_world_t *world,
    ecs_entity_t base,
    ecs_entity_t instance,
    const ecs_instantiate_ctx_t *ctx,
    int32_t depth);

ecs_entity_t flecs_get_prefab_instance_child(
    const ecs_world_t *world,
    ecs_entity_t entity,
    ecs_entity_t prefab_child);

#ifdef FLECS_DEBUG
void flecs_tree_spawner_assert_not_instantiated(
    ecs_world_t *world,
    ecs_entity_t parent);
#else
#define flecs_tree_spawner_assert_not_instantiated(world, parent)
#endif

/* Called during bootstrap to register spawner entities with the world. */
void flecs_bootstrap_prefab(
    ecs_world_t *world);

/* Called during world fini to delete all spawners in the world. */
void flecs_fini_prefab(
    ecs_world_t *world);

#ifdef FLECS_PREFAB

ecs_entity_t flecs_get_on_instantiate_children(
    const ecs_world_t *world,
    const ecs_table_t *table);

void flecs_instantiate_dont_fragment(
    ecs_world_t *world,
    ecs_entity_t base,
    ecs_entity_t instance);

void flecs_instantiate_sparse(
    ecs_world_t *world,
    const ecs_table_range_t *base_child_range,
    const ecs_entity_t *base_children,
    ecs_table_t *instance_table,
    const ecs_entity_t *instance_children,
    int32_t row_offset,
    bool emit_non_sparse);

ecs_entity_t flecs_instantiate_alloc_child_id(
    ecs_world_t *world,
    ecs_entity_t prefab_child,
    ecs_entity_t root_prefab,
    ecs_entity_t root_instance);

EcsTreeSpawner* flecs_prefab_spawner_build(
    ecs_world_t *world,
    ecs_entity_t base);

void flecs_spawner_instantiate(
    ecs_world_t *world,
    EcsTreeSpawner *spawner,
    ecs_entity_t base,
    ecs_entity_t instance,
    const ecs_instantiate_ctx_t *ctx);

void flecs_tree_spawner_fini(
    ecs_world_t *world,
    ecs_entity_t parent);

void EcsTreeSpawner_free(
    EcsTreeSpawner *ptr);

/* Synchronize structural prefab changes (added/removed/reparented children)
 * with all existing instances of the affected prefabs. */
void flecs_prefab_sync(
    ecs_world_t *world);

/* Synchronize structural changes for a single prefab. */
void flecs_prefab_sync_one(
    ecs_world_t *world,
    ecs_entity_t prefab);

/* Mark a prefab (and prefabs that inherit from it) as having structural
 * changes that need to be synchronized with instances. */
void flecs_prefab_mark_dirty(
    ecs_world_t *world,
    ecs_entity_t prefab);

/* Mark prefabs dirty for (ChildOf, prefab) pairs in a type diff. */
void flecs_prefab_mark_dirty_for_child_diff(
    ecs_world_t *world,
    const ecs_type_t *ids);

/* Mark prefabs dirty for (ChildOf, prefab) pairs in a table type. */
void flecs_prefab_mark_dirty_for_child_table(
    ecs_world_t *world,
    const ecs_table_t *table);

/* Called before an entity is deleted, to release cached prefab hierarchy data
 * that references the entity. */
void flecs_prefab_on_delete(
    ecs_world_t *world,
    ecs_entity_t entity);

/* Track the origin of an instance child, so structural changes of the prefab
 * child it was created from can be applied to it. */
void flecs_prefab_set_instance_origin(
    ecs_world_t *world,
    ecs_entity_t instance_child,
    ecs_entity_t prefab_child,
    ecs_entity_t prefab_parent);

/* Remove origin tracking for an instance child. */
void flecs_prefab_remove_instance_origin(
    ecs_world_t *world,
    ecs_entity_t instance_child);

/* Return the prefab child an instance child was created from, or 0. */
ecs_entity_t flecs_prefab_get_instance_origin(
    const ecs_world_t *world,
    ecs_entity_t instance_child);

/* Return whether prefab has structural changes that still need to be synced. */
bool flecs_prefab_is_dirty(
    const ecs_world_t *world,
    ecs_entity_t prefab);

/* Create (or find) the instance child for a prefab child under an instance. */
ecs_entity_t flecs_instantiate_child(
    ecs_world_t *world,
    ecs_entity_t prefab,
    ecs_entity_t instance,
    ecs_entity_t prefab_child);

/* Return the children of a prefab in instantiation order. The returned array
 * must be freed with flecs_wfree_n(). */
int32_t flecs_prefab_sync_children(
    ecs_world_t *world,
    ecs_entity_t prefab,
    ecs_entity_t **out);

/* Return the instance child created from a prefab child, or 0. */
ecs_entity_t flecs_prefab_sync_find_instance_child(
    ecs_world_t *world,
    ecs_entity_t instance,
    ecs_entity_t prefab_child);

/* Invalidate the cached tree spawner for a prefab (and its variants). */
void flecs_tree_spawner_invalidate(
    ecs_world_t *world,
    ecs_entity_t prefab);

#else

#define flecs_tree_spawner_fini(world, parent)
#define flecs_prefab_sync(world)
#define flecs_prefab_sync_one(world, prefab)
#define flecs_prefab_mark_dirty(world, prefab)
#define flecs_prefab_mark_dirty_for_child_diff(world, ids)
#define flecs_prefab_mark_dirty_for_child_table(world, table)
#define flecs_prefab_on_delete(world, entity)
#define flecs_prefab_set_instance_origin(world, ic, pc, pp)
#define flecs_prefab_remove_instance_origin(world, ic)
#define flecs_prefab_get_instance_origin(world, ic) (0)
#define flecs_prefab_is_dirty(world, prefab) (false)
#define flecs_instantiate_child(world, p, i, pc) (0)
#define flecs_prefab_sync_children(world, p, out) (0)
#define flecs_prefab_sync_find_instance_child(world, i, pc) (0)
#define flecs_tree_spawner_invalidate(world, prefab)

#endif


#endif
