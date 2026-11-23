/**
 * @file prefab_sync.c
 * @brief Synchronize structural prefab changes with existing instances.
 *
 * When the hierarchy of a prefab is modified after instances have been created
 * (children added, removed or reparented), the changes have to be propagated to
 * the instances. This file implements that synchronization.
 *
 * Every instance child that is created by prefab instantiation is registered
 * with its origin (the prefab child it was created from, and the prefab parent
 * it was created under). Synchronization uses this information to match
 * instance children with prefab children, which means that:
 * - children that were created by the application are never touched, and
 * - children that were detached from an instance (by reparenting them away)
 *   are never deleted.
 *
 * Structural changes are recorded by marking the affected prefab (and prefabs
 * that inherit from it) as dirty. Dirty prefabs are synchronized at well
 * defined points: when the world is merged (ecs_merge, ecs_readonly_end), at
 * the start of a frame (ecs_progress), lazily when a prefab child is looked up
 * with ecs_get_target(), and when ecs_prefab_sync() is called explicitly.
 * Synchronization is idempotent: calling it multiple times has the same result
 * as calling it once.
 */

#include "../../private_api.h"

#ifdef FLECS_PREFAB

/* Return true if the entity is a prefab. */
static bool flecs_prefab_sync_is_prefab(
    ecs_world_t *world,
    ecs_entity_t entity)
{
    if (!entity || !ecs_is_alive(world, entity)) {
        return false;
    }

    ecs_record_t *r = flecs_entities_get(world, entity);
    if (!r || !r->table) {
        return false;
    }

    return (r->table->flags & EcsTableIsPrefab) != 0;
}

/* Add a prefab to the set of prefabs with pending structural changes. */
static void flecs_prefab_dirty_add(
    ecs_world_t *world,
    ecs_entity_t prefab)
{
    if (!prefab || !ecs_is_alive(world, prefab)) {
        return;
    }

    if (ecs_map_get(&world->prefab_dirty, prefab)) {
        return;
    }

    ecs_map_ensure(&world->prefab_dirty, prefab)[0] = 1;
    world->prefab_sync_pending = true;
}

void flecs_prefab_mark_dirty(
    ecs_world_t *world,
    ecs_entity_t prefab)
{
    if (!flecs_prefab_sync_is_prefab(world, prefab)) {
        return;
    }

    flecs_prefab_dirty_add(world, prefab);

    /* Mark prefabs that inherit from this prefab as dirty as well, since their
     * hierarchy is affected by the change. */
    ecs_component_record_t *cr = flecs_components_get(
        world, ecs_pair(EcsIsA, prefab));
    if (!cr) {
        return;
    }

    ecs_table_cache_iter_t it;
    if (flecs_table_cache_iter(&cr->cache, &it, EcsTableEmpty|EcsTableNotEmpty)) {
        const ecs_table_cache_elem_t *elem;
        while ((elem = flecs_table_cache_next(&it))) {
            ecs_table_t *table = elem->table;
            const ecs_entity_t *entities = ecs_table_entities(table);
            int32_t i, count = ecs_table_count(table);
            for (i = 0; i < count; i ++) {
                ecs_entity_t e = entities[i];
                if (e != prefab && flecs_prefab_sync_is_prefab(world, e)) {
                    flecs_prefab_dirty_add(world, e);
                }
            }
        }
    }
}

bool flecs_prefab_is_dirty(
    const ecs_world_t *world,
    ecs_entity_t prefab)
{
    if (!world->prefab_sync_pending) {
        return false;
    }

    return ecs_map_get(&world->prefab_dirty, prefab) != NULL;
}

void flecs_prefab_set_instance_origin(
    ecs_world_t *world,
    ecs_entity_t instance_child,
    ecs_entity_t prefab_child,
    ecs_entity_t prefab_parent)
{
    (void)prefab_parent;
    ecs_map_ensure(
        &world->prefab_instance_origins, instance_child)[0] = prefab_child;
}

void flecs_prefab_remove_instance_origin(
    ecs_world_t *world,
    ecs_entity_t instance_child)
{
    ecs_map_remove(&world->prefab_instance_origins, instance_child);
}

ecs_entity_t flecs_prefab_get_instance_origin(
    const ecs_world_t *world,
    ecs_entity_t instance_child)
{
    ecs_map_val_t *v = ecs_map_get(
        (ecs_map_t*)&world->prefab_instance_origins, instance_child);
    if (!v) {
        return 0;
    }

    return flecs_entities_get_alive(world, *v);
}

/* Return the raw stored origin, even if the prefab child is no longer alive.
 * Returns 0 if no origin is tracked. */
static ecs_entity_t flecs_prefab_get_instance_origin_raw(
    const ecs_world_t *world,
    ecs_entity_t instance_child)
{
    ecs_map_val_t *v = ecs_map_get(
        (ecs_map_t*)&world->prefab_instance_origins, instance_child);
    if (!v) {
        return 0;
    }

    return (ecs_entity_t)*v;
}

/* Mark prefabs dirty for (ChildOf, prefab) pairs in a type diff. Used when
 * ChildOf-based children are added to or removed from a prefab. */
void flecs_prefab_mark_dirty_for_child_diff(
    ecs_world_t *world,
    const ecs_type_t *ids)
{
    int32_t i, count = ids->count;
    ecs_id_t *array = ids->array;
    for (i = 0; i < count; i ++) {
        ecs_id_t id = array[i];
        if (ECS_IS_PAIR(id) && ECS_PAIR_FIRST(id) == EcsChildOf) {
            ecs_entity_t parent = ECS_PAIR_SECOND(id);
            if (parent && flecs_prefab_sync_is_prefab(world, parent)) {
                flecs_prefab_mark_dirty(world, parent);
            }
        }
    }
}

/* Mark prefabs dirty for (ChildOf, prefab) pairs in a table type. Used when
 * entities are appended directly to a table (for example when a new ChildOf
 * child is created), in which case the diff doesn't contain the ChildOf pair.
 * Only tables whose ChildOf target is a prefab are considered (tracked with the
 * EcsIdPrefabChildren flag). */
void flecs_prefab_mark_dirty_for_child_table(
    ecs_world_t *world,
    const ecs_table_t *table)
{
    ecs_component_record_t *cr = flecs_table_get_childof_cr(world, table);
    if (!cr || !(cr->flags & EcsIdPrefabChildren)) {
        return;
    }

    ecs_entity_t parent = ecs_pair_second(world, cr->id);
    if (parent) {
        flecs_prefab_mark_dirty(world, parent);
    }
}

/* Return the children of a prefab in instantiation order. The returned array
 * must be freed with flecs_wfree_n(). */
int32_t flecs_prefab_sync_children(
    ecs_world_t *world,
    ecs_entity_t prefab,
    ecs_entity_t **out)
{
    *out = NULL;

    ecs_component_record_t *cr = flecs_components_get(
        world, ecs_childof(prefab));
    if (!cr) {
        return 0;
    }

    if (cr->flags & EcsIdOrderedChildren) {
        ecs_vec_t *v = &cr->pair->ordered_children;
        int32_t count = ecs_vec_count(v);
        if (!count) {
            return 0;
        }

        ecs_entity_t *dst = flecs_walloc_n(world, ecs_entity_t, count);
        ecs_os_memcpy_n(dst, ecs_vec_first_t(v, ecs_entity_t),
            ecs_entity_t, count);
        *out = dst;
        return count;
    }

    /* Prefab doesn't use the Parent component, so its children are created
     * with ChildOf pairs and live in tables. Collect them in table order. */
    ecs_vec_t result;
    ecs_vec_init_t(NULL, &result, ecs_entity_t, 0);

    ecs_table_cache_iter_t it;
    if (flecs_table_cache_iter(&cr->cache, &it, EcsTableEmpty|EcsTableNotEmpty)) {
        const ecs_table_cache_elem_t *elem;
        while ((elem = flecs_table_cache_next(&it))) {
            ecs_table_t *table = elem->table;
            const ecs_entity_t *entities = ecs_table_entities(table);
            int32_t i, count = ecs_table_count(table);
            for (i = 0; i < count; i ++) {
                ecs_vec_append_t(NULL, &result, ecs_entity_t)[0] = entities[i];
            }
        }
    }

    int32_t count = ecs_vec_count(&result);
    if (!count) {
        ecs_vec_fini_t(NULL, &result, ecs_entity_t);
        return 0;
    }

    ecs_entity_t *dst = flecs_walloc_n(world, ecs_entity_t, count);
    ecs_os_memcpy_n(dst, ecs_vec_first_t(&result, ecs_entity_t),
        ecs_entity_t, count);
    ecs_vec_fini_t(NULL, &result, ecs_entity_t);
    *out = dst;
    return count;
}

/* Return the first child of an entity that was created from prefab_child. */
ecs_entity_t flecs_prefab_sync_find_instance_child(
    ecs_world_t *world,
    ecs_entity_t instance,
    ecs_entity_t prefab_child)
{
    ecs_entity_t result = 0;

    ecs_component_record_t *cr = flecs_components_get(
        world, ecs_childof(instance));
    if (!cr) {
        return 0;
    }

    if (cr->flags & EcsIdOrderedChildren) {
        ecs_vec_t *v = &cr->pair->ordered_children;
        int32_t i, count = ecs_vec_count(v);
        ecs_entity_t *entities = ecs_vec_first_t(v, ecs_entity_t);
        for (i = 0; i < count; i ++) {
            ecs_entity_t child = entities[i];
            ecs_entity_t origin = flecs_prefab_get_instance_origin_raw(
                world, child);
            if (origin == prefab_child) {
                return child;
            }
        }
    } else {
        ecs_table_cache_iter_t it;
        if (flecs_table_cache_iter(&cr->cache, &it, EcsTableEmpty|EcsTableNotEmpty)) {
            const ecs_table_cache_elem_t *elem;
            while ((elem = flecs_table_cache_next(&it))) {
                ecs_table_t *table = elem->table;
                const ecs_entity_t *entities = ecs_table_entities(table);
                int32_t i, count = ecs_table_count(table);
                for (i = 0; i < count; i ++) {
                    ecs_entity_t child = entities[i];
                    ecs_entity_t origin = flecs_prefab_get_instance_origin_raw(
                        world, child);
                    if (origin == prefab_child) {
                        return child;
                    }
                }
            }
        }
    }

    return result;
}

/* Return true if `prefab` is `parent`, or inherits (transitively) from
 * `parent`. Used to determine whether a prefab child is still reachable. */
static bool flecs_prefab_sync_is_reachable(
    ecs_world_t *world,
    ecs_entity_t prefab,
    ecs_entity_t parent,
    int32_t depth)
{
    if (!parent || !ecs_is_alive(world, parent)) {
        return false;
    }

    if (prefab == parent) {
        return true;
    }

    if (depth >= FLECS_DAG_DEPTH_MAX) {
        return false;
    }

    ecs_component_record_t *cr = flecs_components_get(
        world, ecs_pair(EcsIsA, EcsWildcard));
    (void)cr;

    ecs_record_t *r = flecs_entities_get(world, prefab);
    if (!r || !r->table) {
        return false;
    }

    ecs_table_t *table = r->table;
    if (!(table->flags & EcsTableHasIsA)) {
        return false;
    }

    ecs_component_record_t *isa_wc = world->cr_isa_wildcard;
    const ecs_table_record_t *tr = flecs_component_get_table(isa_wc, table);
    if (!tr) {
        return false;
    }

    int32_t i = tr->index, end = tr->index + tr->count;
    for (; i < end; i ++) {
        ecs_id_t id = table->type.array[i];
        if (ECS_PAIR_FIRST(id) == EcsIsA) {
            ecs_entity_t base = ECS_PAIR_SECOND(id);
            if (flecs_prefab_sync_is_reachable(
                world, base, parent, depth + 1))
            {
                return true;
            }
        }
    }

    return false;
}

/* Return true if an instance child should be removed because the prefab child
 * it was created from is no longer part of the prefab hierarchy. */
static bool flecs_prefab_sync_child_is_stale(
    ecs_world_t *world,
    ecs_entity_t prefab,
    ecs_entity_t child)
{
    ecs_entity_t raw = flecs_prefab_get_instance_origin_raw(world, child);
    if (!raw) {
        return false; /* No tracked origin: application-created child. */
    }

    ecs_entity_t origin = flecs_entities_get_alive(world, raw);
    if (!origin) {
        /* Prefab child was deleted. */
        return true;
    }

    ecs_entity_t parent = ecs_get_parent(world, origin);
    if (flecs_prefab_sync_is_reachable(world, prefab, parent, 0)) {
        /* Prefab child is still reachable from the prefab (either a direct
         * child, or a child of a prefab that this prefab inherits from). */
        return false;
    }

    if (!flecs_prefab_sync_is_prefab(world, parent)) {
        /* Prefab child was reparented to a non-prefab, so it's no longer
         * instantiated. The instance child is detached: leave it alone. */
        return false;
    }

    /* Prefab child was reparented to another prefab. It is recreated under the
     * new parent, so the old instance child is stale. */
    return true;
}

/* Collect all children of an instance whose prefab child is no longer part of
 * the prefab hierarchy. */
static void flecs_prefab_sync_collect_stale(
    ecs_world_t *world,
    ecs_entity_t prefab,
    ecs_entity_t instance,
    ecs_vec_t *stale)
{
    ecs_component_record_t *cr = flecs_components_get(
        world, ecs_childof(instance));
    if (!cr) {
        return;
    }

    if (cr->flags & EcsIdOrderedChildren) {
        ecs_vec_t *v = &cr->pair->ordered_children;
        int32_t i, count = ecs_vec_count(v);
        ecs_entity_t *entities = ecs_vec_first_t(v, ecs_entity_t);
        for (i = 0; i < count; i ++) {
            ecs_entity_t child = entities[i];
            if (flecs_prefab_sync_child_is_stale(world, prefab, child)) {
                ecs_vec_append_t(NULL, stale, ecs_entity_t)[0] = child;
            }
        }
    } else {
        ecs_table_cache_iter_t it;
        if (flecs_table_cache_iter(&cr->cache, &it, EcsTableEmpty|EcsTableNotEmpty)) {
            const ecs_table_cache_elem_t *elem;
            while ((elem = flecs_table_cache_next(&it))) {
                ecs_table_t *table = elem->table;
                const ecs_entity_t *entities = ecs_table_entities(table);
                int32_t i, count = ecs_table_count(table);
                for (i = 0; i < count; i ++) {
                    ecs_entity_t child = entities[i];
                    if (flecs_prefab_sync_child_is_stale(
                        world, prefab, child))
                    {
                        ecs_vec_append_t(NULL, stale, ecs_entity_t)[0] = child;
                    }
                }
            }
        }
    }
}

/* Called before an entity is deleted. If the entity is a prefab child, the
 * prefab it belongs to is marked for synchronization, and its cached hierarchy
 * data is released (the tree spawner references the tables that the prefab
 * child lives in). The synchronization itself happens after the delete has
 * completed (when the world is merged, or explicitly with ecs_prefab_sync()). */
void flecs_prefab_on_delete(
    ecs_world_t *world,
    ecs_entity_t entity)
{
    /* Release origin tracking for the entity, so that a recycled entity id
     * doesn't get mistaken for an instance child. */
    flecs_prefab_remove_instance_origin(world, entity);

    if (world->flags & (EcsWorldFini|EcsWorldQuit)) {
        return;
    }

    ecs_entity_t parent = ecs_get_parent(world, entity);
    if (!parent) {
        return;
    }

    /* Only prefab children have cached hierarchy data that references them. */
    if (!flecs_prefab_sync_is_prefab(world, parent)) {
        return;
    }

    /* Mark the prefab as dirty so that removing a child is synchronized with
     * instances (also for children that use ChildOf pairs). */
    flecs_prefab_mark_dirty(world, parent);
    flecs_tree_spawner_invalidate(world, parent);
}

/* Remove children of an instance that no longer have a corresponding prefab
 * child. Children that were detached from the instance are left alive. */
static void flecs_prefab_sync_remove_stale(
    ecs_world_t *world,
    ecs_entity_t prefab,
    ecs_entity_t instance)
{
    ecs_vec_t stale;
    ecs_vec_init_t(NULL, &stale, ecs_entity_t, 0);

    flecs_prefab_sync_collect_stale(world, prefab, instance, &stale);

    int32_t i, count = ecs_vec_count(&stale);
    ecs_entity_t *entities = ecs_vec_first_t(&stale, ecs_entity_t);
    for (i = 0; i < count; i ++) {
        ecs_entity_t e = entities[i];
        if (!ecs_is_alive(world, e)) {
            continue;
        }

        /* Only delete children that are still a child of the instance. A
         * detached child must not be deleted. */
        if (ecs_get_parent(world, e) != instance) {
            flecs_prefab_remove_instance_origin(world, e);
            continue;
        }

        flecs_prefab_remove_instance_origin(world, e);
        ecs_delete(world, e);
    }

    ecs_vec_fini_t(NULL, &stale, ecs_entity_t);
}

/* Create (or find) the instance child for a prefab child under an instance.
 * Used when a prefab child is added to a prefab after instances have been
 * created. The new instance child is created with the same IsA relationship to
 * the prefab child as a regular instantiated child, so it inherits/owns the
 * same components. */
ecs_entity_t flecs_instantiate_child(
    ecs_world_t *world,
    ecs_entity_t prefab,
    ecs_entity_t instance,
    ecs_entity_t prefab_child)
{
    ecs_check(ecs_is_alive(world, prefab), ECS_INVALID_PARAMETER, NULL);
    ecs_check(ecs_is_alive(world, instance), ECS_INVALID_PARAMETER, NULL);
    ecs_check(ecs_is_alive(world, prefab_child), ECS_INVALID_PARAMETER, NULL);

    /* Return existing instance child, if any. */
    ecs_entity_t existing = flecs_prefab_sync_find_instance_child(
        world, instance, prefab_child);
    if (existing) {
        return existing;
    }

    /* Match the hierarchy mechanism that the prefab used. If the prefab's
     * children use the Parent component, create a Parent child. Otherwise
     * create a ChildOf child. */
    const char *name = ecs_get_name(world, prefab_child);
    bool use_parent = false;
    ecs_component_record_t *prefab_childof = flecs_components_get(
        world, ecs_childof(prefab));
    if (prefab_childof && (prefab_childof->flags & EcsIdOrderedChildren)) {
        use_parent = true;
    }

    ecs_entity_t result;
    if (use_parent) {
        result = ecs_new_w_parent(world, instance, name);
    } else {
        result = ecs_new(world);
        if (name) {
            ecs_set_name(world, result, name);
        }
        ecs_add_pair(world, result, EcsChildOf, instance);
    }

    ecs_add_pair(world, result, EcsIsA, prefab_child);

    flecs_prefab_set_instance_origin(
        world, result, prefab_child, prefab);

    /* Recursively create children of the new prefab child. */
    ecs_entity_t *prefab_children = NULL;
    int32_t i, count = flecs_prefab_sync_children(
        world, prefab_child, &prefab_children);
    for (i = 0; i < count; i ++) {
        flecs_instantiate_child(
            world, prefab_child, result, prefab_children[i]);
    }

    if (prefab_children) {
        flecs_wfree_n(world, ecs_entity_t, count, prefab_children);
    }

    return result;
error:
    return 0;
}

/* Reconcile the children of an instance with the children of a prefab. */
static void flecs_prefab_sync_instance(
    ecs_world_t *world,
    ecs_entity_t prefab,
    ecs_entity_t instance,
    int32_t depth)
{
    ecs_check(depth < FLECS_DAG_DEPTH_MAX, ECS_CYCLE_DETECTED,
        "cycle detected while synchronizing prefab hierarchy");

    /* If the prefab doesn't instantiate children, there is nothing to sync. */
    ecs_record_t *r = flecs_entities_get(world, prefab);
    if (!r || !r->table) {
        return;
    }

    ecs_entity_t on_instantiate = flecs_get_on_instantiate_children(
        world, r->table);
    if (on_instantiate == EcsDontInherit) {
        return;
    }

    ecs_entity_t *prefab_children = NULL;
    int32_t prefab_child_count = flecs_prefab_sync_children(
        world, prefab, &prefab_children);

    int32_t i;
    for (i = 0; i < prefab_child_count; i ++) {
        ecs_entity_t prefab_child = prefab_children[i];

        /* Skip children that shouldn't be instantiated (for example when a
         * prefab child no longer has the Prefab tag). */
        if (!flecs_prefab_sync_is_prefab(world, prefab_child)) {
            continue;
        }

        ecs_entity_t instance_child = flecs_prefab_sync_find_instance_child(
            world, instance, prefab_child);

        if (!instance_child) {
            instance_child = flecs_instantiate_child(
                world, prefab, instance, prefab_child);
        }

        if (!instance_child) {
            continue;
        }

        flecs_prefab_sync_instance(
            world, prefab_child, instance_child, depth + 1);
    }

    if (prefab_children) {
        flecs_wfree_n(world, ecs_entity_t, prefab_child_count, prefab_children);
    }
error:
    return;
}

/* Synchronize a single prefab with all its instances. */
void flecs_prefab_sync_one(
    ecs_world_t *world,
    ecs_entity_t prefab)
{
    if (!flecs_prefab_sync_is_prefab(world, prefab)) {
        ecs_map_remove(&world->prefab_dirty, prefab);
        return;
    }

    ecs_map_remove(&world->prefab_dirty, prefab);
    if (!ecs_map_count(&world->prefab_dirty)) {
        world->prefab_sync_pending = false;
    }

    /* Invalidate the cached tree spawner so that new instances are created
     * from the current hierarchy. */
    flecs_tree_spawner_invalidate(world, prefab);

    ecs_component_record_t *cr = flecs_components_get(
        world, ecs_pair(EcsIsA, prefab));
    if (!cr) {
        return;
    }

    /* Collect instances first: synchronization modifies tables, which can
     * invalidate the table cache that is being iterated. */
    ecs_vec_t instances;
    ecs_vec_init_t(NULL, &instances, ecs_entity_t, 0);

    ecs_table_cache_iter_t it;
    if (flecs_table_cache_iter(&cr->cache, &it, EcsTableEmpty|EcsTableNotEmpty)) {
        const ecs_table_cache_elem_t *elem;
        while ((elem = flecs_table_cache_next(&it))) {
            ecs_table_t *table = elem->table;
            const ecs_entity_t *entities = ecs_table_entities(table);
            int32_t i, count = ecs_table_count(table);
            for (i = 0; i < count; i ++) {
                ecs_entity_t instance = entities[i];
                if (instance == prefab) {
                    continue;
                }

                ecs_record_t *r = flecs_entities_get(world, instance);
                if (!r || !r->table) {
                    continue;
                }

                ecs_vec_append_t(NULL, &instances, ecs_entity_t)[0] = instance;
            }
        }
    }

    int32_t i, count = ecs_vec_count(&instances);
    ecs_entity_t *entities = ecs_vec_first_t(&instances, ecs_entity_t);
    for (i = 0; i < count; i ++) {
        ecs_entity_t instance = entities[i];
        if (!ecs_is_alive(world, instance)) {
            continue;
        }

        ecs_record_t *r = flecs_entities_get(world, instance);
        if (!r || !r->table) {
            continue;
        }

        flecs_prefab_sync_remove_stale(world, prefab, instance);
        flecs_prefab_sync_instance(world, prefab, instance, 0);

        if (r->table->flags & EcsTableIsPrefab) {
            /* Variants are instances of the base prefab. Mark the variant and
             * its instances as dirty so that the changes propagate to the
             * variant's hierarchy and to the instances of the variant. */
            flecs_prefab_dirty_add(world, instance);
            world->prefab_sync_pending = true;
        }
    }

    ecs_vec_fini_t(NULL, &instances, ecs_entity_t);
}

/* Synchronize all prefabs that have pending structural changes. */
void flecs_prefab_sync(
    ecs_world_t *world)
{
    if (!world->prefab_sync_pending || world->prefab_sync_active) {
        return;
    }

    if (world->flags & (EcsWorldFini|EcsWorldQuit|EcsWorldReadonly)) {
        return;
    }

    world->prefab_sync_active = true;

    int32_t i;
    do {
        ecs_vec_t prefabs;
        ecs_vec_init_t(NULL, &prefabs, ecs_entity_t, 0);

        ecs_map_iter_t mit = ecs_map_iter(&world->prefab_dirty);
        while (ecs_map_next(&mit)) {
            ecs_entity_t prefab = (ecs_entity_t)ecs_map_key(&mit);
            if (ecs_is_alive(world, prefab)) {
                ecs_vec_append_t(NULL, &prefabs, ecs_entity_t)[0] = prefab;
            } else {
                ecs_map_remove(&world->prefab_dirty, prefab);
            }
        }

        int32_t count = ecs_vec_count(&prefabs);
        if (!count) {
            ecs_vec_fini_t(NULL, &prefabs, ecs_entity_t);
            break;
        }

        /* Synchronize base prefabs before variants. Variants have IsA
         * relationships to the prefabs they inherit from, so sorting by depth
         * ensures a variant is synchronized after its base. */
        ecs_entity_t *entities = ecs_vec_first_t(&prefabs, ecs_entity_t);
        for (i = 0; i < count; i ++) {
            int32_t j;
            for (j = i + 1; j < count; j ++) {
                int32_t di = ecs_get_depth(world, entities[i], EcsIsA);
                int32_t dj = ecs_get_depth(world, entities[j], EcsIsA);
                if (dj < di) {
                    ecs_entity_t tmp = entities[i];
                    entities[i] = entities[j];
                    entities[j] = tmp;
                }
            }
        }

        for (i = 0; i < count; i ++) {
            if (ecs_is_alive(world, entities[i])) {
                flecs_prefab_sync_one(world, entities[i]);
            }
        }

        ecs_vec_fini_t(NULL, &prefabs, ecs_entity_t);
    } while (ecs_map_count(&world->prefab_dirty) != 0);

    world->prefab_sync_pending = false;
    world->prefab_sync_active = false;
}

/* Public API implementation. */
void ecs_prefab_sync(
    ecs_world_t *world)
{
    ecs_check(world != NULL, ECS_INVALID_PARAMETER, NULL);
    world = ECS_CONST_CAST(ecs_world_t*, ecs_get_world(world));
    flecs_prefab_sync(world);
error:
    return;
}

bool ecs_prefab_sync_pending(
    const ecs_world_t *world)
{
    ecs_check(world != NULL, ECS_INVALID_PARAMETER, NULL);
    world = ecs_get_world(world);
    return world->prefab_sync_pending;
error:
    return false;
}

#else

/* Public API stubs. The internal flecs_prefab_* functions are provided as
 * macros by prefab.h when FLECS_PREFAB is disabled, so no definitions are
 * needed for them (defining them would collide with the macros). */
void ecs_prefab_sync(
    ecs_world_t *world)
{
    (void)world;
}

bool ecs_prefab_sync_pending(
    const ecs_world_t *world)
{
    (void)world;
    return false;
}

#endif
