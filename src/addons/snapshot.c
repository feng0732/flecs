/**
 * @file addons/snapshot.c
 * @brief World state snapshots and restore.
 *
 * A snapshot captures the complete, user visible state of a world so that it
 * can be restored later. The central guarantee is reproducibility: after a
 * restore, continuing to run the world with the same input sequence produces
 * the same state as the original run. To achieve this a snapshot stores:
 *
 * - the entity id index, i.e. which ids are alive, their generation, the order
 *   in which ids were issued and the max issued id. The generation of an
 *   entity is stored in the dense array of the entity index, so restoring the
 *   dense array restores generations of recycled ids exactly;
 * - for every alive user entity, the ids it owns (including toggle ids that
 *   encode enabled/disabled state) and the data of owned components;
 * - the hierarchy, which is part of the owned ids: (ChildOf, parent) pairs for
 *   fragmenting hierarchies and the EcsParent component for non-fragmenting
 *   hierarchies.
 *
 * Restore is implemented as a diff between the snapshot and the current world.
 * Only entities/components that actually differ are changed, so restoring an
 * identical snapshot (or restoring the same snapshot twice) is a no-op that
 * does not invoke any lifecycle hook or observer. This also means that
 * observers, queries and systems that were registered before the snapshot was
 * taken keep working after restore: their (builtin, poly) entities are never
 * captured and are never removed by restore.
 *
 * Schema entities (builtin entities and component/relationship registration
 * entities in the low id range) are not captured. They describe the structure
 * of the world (component registration, type info) rather than simulation
 * state and must stay intact for restore to be able to reconstruct owned ids.
 */

#include "../private_api.h"

#ifdef FLECS_SNAPSHOT

#define FLECS_SNAPSHOT_MAGIC       (0x45435353u) /* "ECSS" */
#define FLECS_SNAPSHOT_VERSION     (1u)

/* Header at the start of every snapshot. */
typedef struct ecs_snapshot_header_t {
    uint32_t magic;
    uint32_t version;
    uint64_t id;
} ecs_snapshot_header_t;

/* A single captured component id for an entity. */
typedef struct ecs_snapshot_id_t {
    ecs_id_t id;              /* Stored id (may include ECS_TOGGLE). */
    void *data;               /* Deep-copied component value, or NULL. */
    ecs_size_t size;          /* Size of captured data (0 for tags/toggles). */
    const ecs_type_info_t *ti;/* Type info owning the value (claimed). */
    bool is_toggle;           /* Id is a toggle id. */
    bool enabled;             /* Enabled state, for toggle ids. */
} ecs_snapshot_id_t;

/* Per-entity captured state. */
typedef struct ecs_snapshot_entity_t {
    ecs_entity_t entity;      /* Entity id (with generation). */
    ecs_vec_t ids;            /* vec<ecs_snapshot_id_t> */
} ecs_snapshot_entity_t;

/* Per-table captured state, for introspection. */
typedef struct ecs_snapshot_table_t {
    uint64_t table_id;
    ecs_type_t type;
    int32_t count;
} ecs_snapshot_table_t;

struct ecs_snapshot_t {
    ecs_snapshot_header_t hdr;

    /* Entity index state. */
    uint64_t *dense;
    int32_t dense_count;
    int32_t alive_count;
    uint32_t max_id;

    /* Map<entity_id, 1> for O(1) membership tests (key: low 32 bits). */
    ecs_map_t entity_map;

    /* Captured entities, in dense (issue) order. */
    ecs_vec_t entities;       /* vec<ecs_snapshot_entity_t> */
    ecs_vec_t tables;         /* vec<ecs_snapshot_table_t> */
};

/* Registry of live snapshots so that dangling/freed handles are detectable
 * without dereferencing freed memory. Keyed by pointer value. */
static ecs_map_t flecs_snapshot_live;
static bool flecs_snapshot_live_init = false;

static void flecs_snapshot_register(
    const ecs_snapshot_t *snapshot)
{
    if (!flecs_snapshot_live_init) {
        ecs_map_init(&flecs_snapshot_live, NULL);
        flecs_snapshot_live_init = true;
    }
    ecs_map_insert(&flecs_snapshot_live,
        ECS_CAST(ecs_map_key_t, ECS_PTR_CAST(uintptr_t, snapshot)), 1);
}

static void flecs_snapshot_unregister(
    const ecs_snapshot_t *snapshot)
{
    if (!flecs_snapshot_live_init) {
        return;
    }
    ecs_map_remove(&flecs_snapshot_live,
        ECS_CAST(ecs_map_key_t, ECS_PTR_CAST(uintptr_t, snapshot)));
}

static bool flecs_snapshot_is_live(
    const ecs_snapshot_t *snapshot)
{
    if (!snapshot || !flecs_snapshot_live_init) {
        return false;
    }
    return ecs_map_get(&flecs_snapshot_live,
        ECS_CAST(ecs_map_key_t, ECS_PTR_CAST(uintptr_t, snapshot))) != NULL;
}

/* -- Result string -- */

const char* ecs_snapshot_result_str(
    ecs_snapshot_result_t result)
{
    switch (result) {
    case EcsSnapshotOk: return "Ok";
    case EcsSnapshotNoChanges: return "NoChanges";
    case EcsSnapshotError: return "Error";
    case EcsSnapshotInvalidSnapshot: return "InvalidSnapshot";
    case EcsSnapshotInvalidWorld: return "InvalidWorld";
    case EcsSnapshotInvalidOperation: return "InvalidOperation";
    case EcsSnapshotEmpty: return "Empty";
    case EcsSnapshotInvalidHandle: return "InvalidHandle";
    case EcsSnapshotVersionMismatch: return "VersionMismatch";
    default: return "Unknown";
    }
}

/* -- Public introspection -- */

bool ecs_snapshot_is_valid(
    const ecs_snapshot_t *snapshot)
{
    return flecs_snapshot_is_live(snapshot) &&
        snapshot->hdr.magic == FLECS_SNAPSHOT_MAGIC;
}

uint64_t ecs_snapshot_id(
    const ecs_snapshot_t *snapshot)
{
    if (!ecs_snapshot_is_valid(snapshot)) {
        return 0;
    }
    return snapshot->hdr.id;
}

int32_t ecs_snapshot_entity_count(
    const ecs_snapshot_t *snapshot)
{
    if (!ecs_snapshot_is_valid(snapshot)) {
        return -1;
    }
    return ecs_vec_count(&snapshot->entities);
}

int32_t ecs_snapshot_table_count(
    const ecs_snapshot_t *snapshot)
{
    if (!ecs_snapshot_is_valid(snapshot)) {
        return -1;
    }
    return ecs_vec_count(&snapshot->tables);
}

/* -- Helpers -- */

/* Return the root ancestor of an entity, following ChildOf and EcsParent. */
static ecs_entity_t flecs_snapshot_root_ancestor(
    ecs_world_t *world,
    ecs_entity_t e)
{
    int32_t guard = 0;
    while (guard ++ < 1024) {
        ecs_entity_t parent = ecs_get_target(world, e, EcsChildOf, 0);
        if (!parent) {
            const EcsParent *p = ecs_get(world, e, EcsParent);
            if (p) {
                parent = p->value;
            }
        }
        if (!parent || parent == e) {
            return e;
        }
        e = parent;
    }
    return e;
}

/* Whether an entity is part of the restorable user state. Schema entities
 * (registered components, builtin entities, meta types, modules) and poly
 * entities (observers, queries, systems) are excluded: they describe the
 * structure of the world rather than simulation state, and restoring them
 * would either be a no-op or tear down registrations the world depends on. */
static bool flecs_snapshot_entity_is_user(
    ecs_world_t *world,
    ecs_entity_t e)
{
    if (!e) {
        return false;
    }

    if ((uint32_t)e <= EcsFirstUserEntityId) {
        return false;
    }

    /* Component/relationship registration (schema). */
    if (ecs_has_id(world, e, ecs_id(EcsComponent))) {
        return false;
    }

    /* Poly objects (observers, queries, systems). */
    if (ecs_has_pair(world, e, ecs_id(EcsPoly), EcsAny)) {
        return false;
    }

    /* Modules and anything else living inside the flecs module tree (meta
     * types, pipeline systems, script templates, ...) is framework state. */
    if (ecs_has_id(world, e, EcsModule)) {
        return false;
    }
    ecs_entity_t root = flecs_snapshot_root_ancestor(world, e);
    if (root == EcsFlecs || root == EcsFlecsCore) {
        return false;
    }

    return true;
}

static bool flecs_snapshot_contains(
    const ecs_snapshot_t *snapshot,
    ecs_entity_t entity)
{
    ecs_map_val_t *v = ecs_map_get(&snapshot->entity_map,
        ECS_CAST(ecs_map_key_t, (uint32_t)entity));
    if (!v) {
        return false;
    }
    /* Stored value is the full entity id (with generation), so that recycled
     * ids are matched by generation and not treated as the snapshot entity. */
    return v[0] == ECS_CAST(ecs_map_val_t, entity);
}

/* -- Take -- */

/* Capture the value of a single (component) id for an entity into a snapshot id
 * entry. Deep-copies the value through the type hooks so that resource-owning
 * components (strings, containers, ...) remain valid even after the world
 * destroys the original. */
static void flecs_snapshot_capture_id(
    ecs_world_t *world,
    ecs_entity_t e,
    const ecs_record_t *r,
    ecs_snapshot_id_t *sid,
    ecs_component_record_t *cr,
    ecs_table_t *table)
{
    const ecs_type_info_t *ti = cr->type_info;
    if (!ti || !ti->size) {
        return;
    }

    void *ptr = NULL;
    if (cr->flags & EcsIdSparse) {
        ptr = flecs_component_sparse_get(world, cr, table, e);
    } else {
        const ecs_table_record_t *tr = flecs_component_get_table(cr, table);
        if (!tr || tr->column == -1) {
            return;
        }
        ptr = ECS_ELEM(table->data.columns[tr->column].data, ti->size,
            ECS_RECORD_TO_ROW(r->row));
    }

    if (!ptr) {
        return;
    }

    void *dst = ecs_os_malloc(ti->size);
    if (ti->hooks.copy_ctor) {
        flecs_type_info_copy_ctor(dst, ptr, 1, ti);
    } else {
        ecs_os_memcpy(dst, ptr, ti->size);
    }

    sid->size = ti->size;
    sid->data = dst;
    sid->ti = ti;
    flecs_type_info_claim(ti);
}

static void flecs_snapshot_capture_entity(
    ecs_world_t *world,
    ecs_snapshot_t *snapshot,
    ecs_entity_t e)
{
    ecs_record_t *r = flecs_entities_get(world, e);
    if (!r || !r->table) {
        return;
    }

    ecs_table_t *table = r->table;
    int32_t i, count = table->type.count;

    ecs_snapshot_entity_t *se = ecs_vec_append_t(
        NULL, &snapshot->entities, ecs_snapshot_entity_t);
    ecs_os_zeromem(se);
    se->entity = e;
    ecs_vec_init_t(NULL, &se->ids, ecs_snapshot_id_t, count);
    ecs_map_insert(&snapshot->entity_map,
        ECS_CAST(ecs_map_key_t, (uint32_t)e), ECS_CAST(ecs_map_val_t, e));

    for (i = 0; i < count; i ++) {
        ecs_id_t id = table->type.array[i];

        /* EcsParentDepth is derived from the EcsParent hierarchy and is
         * re-created by the Parent hook, so it is not captured. */
        if (ECS_IS_PAIR(id) && ECS_PAIR_FIRST(id) == EcsParentDepth &&
            ECS_PAIR_SECOND(id) != EcsWildcard)
        {
            continue;
        }

        ecs_snapshot_id_t *sid = ecs_vec_append_t(
            NULL, &se->ids, ecs_snapshot_id_t);
        ecs_os_zeromem(sid);
        sid->id = id;

        if (ECS_HAS_ID_FLAG(id, TOGGLE)) {
            sid->is_toggle = true;
            sid->enabled = ecs_is_enabled_id(world, e, id & ~ECS_TOGGLE);
            continue;
        }

        ecs_component_record_t *cr = flecs_components_get(world, id);
        if (!cr) {
            continue;
        }

        flecs_snapshot_capture_id(world, e, r, sid, cr, table);
    }

    /* Capture non-fragmenting (DontFragment) components. These are stored in
     * sparse storage and are not part of the table type, so they must be
     * captured separately. */
    if (r->row & EcsEntityHasDontFragment) {
        ecs_component_record_t *cur = world->cr_non_fragmenting_head;
        while (cur) {
            ecs_component_record_t *next = cur->non_fragmenting.next;
            if (!ecs_id_is_wildcard(cur->id) && cur->sparse &&
                flecs_component_sparse_has(cur, e))
            {
                ecs_snapshot_id_t *sid = ecs_vec_append_t(
                    NULL, &se->ids, ecs_snapshot_id_t);
                ecs_os_zeromem(sid);
                sid->id = cur->id;
                flecs_snapshot_capture_id(world, e, r, sid, cur, table);
            }
            cur = next;
        }
    }
}

static void flecs_snapshot_capture_tables(
    ecs_world_t *world,
    ecs_snapshot_t *snapshot)
{
    int32_t count = flecs_sparse_count(&world->store.tables);
    int32_t i;
    for (i = 0; i < count; i ++) {
        ecs_table_t *table = flecs_sparse_get_dense_t(
            &world->store.tables, ecs_table_t, i);
        if (!ecs_table_count(table)) {
            continue;
        }

        ecs_snapshot_table_t *st = ecs_vec_append_t(
            NULL, &snapshot->tables, ecs_snapshot_table_t);
        ecs_os_zeromem(st);
        st->table_id = table->id;
        st->count = ecs_table_count(table);
        st->type.count = table->type.count;
        if (st->type.count) {
            st->type.array = ecs_os_malloc_n(ecs_id_t, st->type.count);
            ecs_os_memcpy_n(st->type.array, table->type.array, ecs_id_t,
                st->type.count);
        }
    }
}

/* Return true if any command queue (across stages) has pending commands. */
static bool flecs_snapshot_has_pending_commands(
    ecs_world_t *world)
{
    int32_t i, count = world->stage_count;
    for (i = 0; i < count; i ++) {
        ecs_stage_t *stage = world->stages[i];
        for (ecs_commands_t *cmd = stage->cmd; cmd; cmd = cmd->prev) {
            if (ecs_vec_count(&cmd->queue)) {
                return true;
            }
        }
    }
    return false;
}

ecs_snapshot_t* ecs_snapshot_take(
    ecs_world_t *world)
{
    if (!world) {
        ecs_err("ecs_snapshot_take: NULL world");
        return NULL;
    }

    ecs_world_t *w = ECS_CONST_CAST(ecs_world_t*, ecs_get_world(world));
    if (!flecs_poly_is(w, ecs_world_t)) {
        ecs_err("ecs_snapshot_take: invalid world");
        return NULL;
    }

    if (w->flags & (EcsWorldReadonly|EcsWorldMultiThreaded|EcsWorldFini)) {
        ecs_err("ecs_snapshot_take: cannot snapshot world while in "
            "readonly/multithreaded/fini mode");
        return NULL;
    }
    if (w->flush_depth) {
        ecs_err("ecs_snapshot_take: cannot snapshot world while commands are "
            "being flushed");
        return NULL;
    }
    if (flecs_snapshot_has_pending_commands(w)) {
        ecs_err("ecs_snapshot_take: cannot snapshot world with pending "
            "commands (merge commands first, e.g. by ending the frame)");
        return NULL;
    }

    flecs_check_exclusive_world_access_write(w);

    ecs_snapshot_t *result = ecs_os_calloc_t(ecs_snapshot_t);
    ecs_assert(result != NULL, ECS_OUT_OF_MEMORY, NULL);

    static uint64_t flecs_snapshot_last_id = 0;
    result->hdr.magic = FLECS_SNAPSHOT_MAGIC;
    result->hdr.version = FLECS_SNAPSHOT_VERSION;
    result->hdr.id = ++ flecs_snapshot_last_id;

    flecs_snapshot_register(result);

    result->dense = flecs_entity_index_copy_dense(
        ecs_eis(w), &result->dense_count);
    result->alive_count = flecs_entity_index_count(ecs_eis(w)) + 1;
    result->max_id = (uint32_t)flecs_entities_max_id(w);

    ecs_map_init(&result->entity_map, NULL);
    ecs_vec_init_t(NULL, &result->entities, ecs_snapshot_entity_t, 0);
    ecs_vec_init_t(NULL, &result->tables, ecs_snapshot_table_t, 0);

    flecs_snapshot_capture_tables(w, result);

    int32_t i;
    const uint64_t *dense = result->dense;
    for (i = 1; i < result->alive_count; i ++) {
        ecs_entity_t e = dense[i];
        if (flecs_snapshot_entity_is_user(w, e)) {
            flecs_snapshot_capture_entity(w, result, e);
        }
    }

    return result;
}

/* -- Free -- */

void ecs_snapshot_free(
    ecs_snapshot_t *snapshot)
{
    if (!ecs_snapshot_is_valid(snapshot)) {
        return;
    }

    flecs_snapshot_unregister(snapshot);
    snapshot->hdr.magic = 0;

    int32_t i, count = ecs_vec_count(&snapshot->entities);
    for (i = 0; i < count; i ++) {
        ecs_snapshot_entity_t *se = ecs_vec_get_t(
            &snapshot->entities, ecs_snapshot_entity_t, i);
        int32_t j, id_count = ecs_vec_count(&se->ids);
        for (j = 0; j < id_count; j ++) {
            ecs_snapshot_id_t *sid = ecs_vec_get_t(
                &se->ids, ecs_snapshot_id_t, j);
            if (sid->data) {
                if (sid->ti->hooks.dtor) {
                    flecs_type_info_dtor(sid->data, 1, sid->ti);
                }
                ecs_os_free(sid->data);
                flecs_type_info_release(sid->ti);
            }
        }
        ecs_vec_fini_t(NULL, &se->ids, ecs_snapshot_id_t);
    }
    ecs_vec_fini_t(NULL, &snapshot->entities, ecs_snapshot_entity_t);

    count = ecs_vec_count(&snapshot->tables);
    for (i = 0; i < count; i ++) {
        ecs_snapshot_table_t *st = ecs_vec_get_t(
            &snapshot->tables, ecs_snapshot_table_t, i);
        ecs_os_free(st->type.array);
    }
    ecs_vec_fini_t(NULL, &snapshot->tables, ecs_snapshot_table_t);

    ecs_map_fini(&snapshot->entity_map);
    ecs_os_free(snapshot->dense);
    ecs_os_free(snapshot);
}

/* -- Restore -- */

/* Structurally validate the snapshot against the world. */
static ecs_snapshot_result_t flecs_snapshot_validate(
    ecs_world_t *world,
    const ecs_snapshot_t *snapshot)
{
    if (snapshot->hdr.version != FLECS_SNAPSHOT_VERSION) {
        return EcsSnapshotVersionMismatch;
    }

    int32_t i, count = ecs_vec_count(&snapshot->entities);
    for (i = 0; i < count; i ++) {
        ecs_snapshot_entity_t *se = ecs_vec_get_t(
            &snapshot->entities, ecs_snapshot_entity_t, i);

        int32_t j, id_count = ecs_vec_count(&se->ids);
        for (j = 0; j < id_count; j ++) {
            ecs_snapshot_id_t *sid = ecs_vec_get_t(
                &se->ids, ecs_snapshot_id_t, j);
            ecs_id_t id = sid->id;
            if (sid->is_toggle) {
                id &= ~ECS_TOGGLE;
            }

            ecs_entity_t rel = ECS_IS_PAIR(id)
                ? ECS_PAIR_FIRST(id) : (id & ECS_COMPONENT_MASK);
            if (!flecs_entities_exists(world, rel)) {
                return EcsSnapshotVersionMismatch;
            }

            if (ECS_IS_PAIR(id) && !ECS_IS_VALUE_PAIR(id)) {
                ecs_entity_t tgt = ECS_PAIR_SECOND(id);
                if (tgt && tgt != EcsWildcard &&
                    !flecs_entities_exists(world, tgt) &&
                    !flecs_snapshot_contains(snapshot, tgt))
                {
                    /* Pair target neither exists nor is restored by the
                     * snapshot: the snapshot references a dangling handle. */
                    return EcsSnapshotInvalidHandle;
                }
            }
        }
    }

    return EcsSnapshotOk;
}

static ecs_snapshot_id_t* flecs_snapshot_entity_find(
    ecs_snapshot_entity_t *se,
    ecs_id_t id)
{
    int32_t i, count = ecs_vec_count(&se->ids);
    for (i = 0; i < count; i ++) {
        ecs_snapshot_id_t *sid = ecs_vec_get_t(&se->ids, ecs_snapshot_id_t, i);
        ecs_id_t cmp = sid->is_toggle ? (sid->id & ~ECS_TOGGLE) : sid->id;
        if (cmp == id) {
            return sid;
        }
    }
    return NULL;
}

static bool flecs_snapshot_data_eq(
    ecs_world_t *world,
    ecs_entity_t e,
    ecs_snapshot_id_t *sid)
{
    if (sid->is_toggle) {
        return ecs_is_enabled_id(world, e, sid->id & ~ECS_TOGGLE) ==
            sid->enabled;
    }

    if (!sid->size) {
        return ecs_has_id(world, e, sid->id);
    }

    const void *ptr = ecs_get_id(world, e, sid->id);
    if (!ptr) {
        return false;
    }

    /* (Identifier, *) values contain heap pointers that are re-allocated on
     * copy, so compare by value. */
    if (ECS_IS_PAIR(sid->id) && ECS_PAIR_FIRST(sid->id) == ecs_id(EcsIdentifier)) {
        const EcsIdentifier *a = ptr, *b = sid->data;
        if ((a->value == NULL) != (b->value == NULL)) {
            return false;
        }
        if (a->value && ecs_os_strcmp(a->value, b->value)) {
            return false;
        }
        return true;
    }

    if (sid->ti->hooks.equals &&
        !(sid->ti->hooks.flags & ECS_TYPE_HOOK_EQUALS_ILLEGAL))
    {
        return flecs_type_info_equals(ptr, sid->data, sid->ti);
    }

    return !ecs_os_memcmp(ptr, sid->data, sid->size);
}

static bool flecs_snapshot_matches(
    ecs_world_t *world,
    ecs_snapshot_t *snapshot)
{
    ecs_entities_t entities = ecs_get_entities(world);
    int32_t i;
    for (i = 0; i < entities.alive_count; i ++) {
        ecs_entity_t e = entities.ids[i];
        if (!flecs_snapshot_entity_is_user(world, e)) {
            continue;
        }
        if (!flecs_snapshot_contains(snapshot, e)) {
            return false;
        }
    }

    int32_t s, count = ecs_vec_count(&snapshot->entities);
    for (s = 0; s < count; s ++) {
        ecs_snapshot_entity_t *se = ecs_vec_get_t(
            &snapshot->entities, ecs_snapshot_entity_t, s);
        ecs_entity_t e = se->entity;
        if (!ecs_is_alive(world, e)) {
            return false;
        }

        int32_t j, id_count = ecs_vec_count(&se->ids);
        for (j = 0; j < id_count; j ++) {
            ecs_snapshot_id_t *sid = ecs_vec_get_t(
                &se->ids, ecs_snapshot_id_t, j);
            if (!flecs_snapshot_data_eq(world, e, sid)) {
                return false;
            }
        }

        const ecs_type_t *type = ecs_get_type(world, e);
        if (type) {
            int32_t t;
            for (t = 0; t < type->count; t ++) {
                ecs_id_t id = type->array[t];
                if (ECS_IS_PAIR(id) && ECS_PAIR_FIRST(id) == EcsParentDepth &&
                    ECS_PAIR_SECOND(id) != EcsWildcard)
                {
                    continue;
                }
                ecs_id_t cmp = ECS_HAS_ID_FLAG(id, TOGGLE)
                    ? (id & ~ECS_TOGGLE) : id;
                if (!flecs_snapshot_entity_find(se, cmp)) {
                    return false;
                }
            }
        }

        /* Non-fragmenting components are not part of the table type. */
        ecs_record_t *r = flecs_entities_get(world, e);
        if (r && (r->row & EcsEntityHasDontFragment)) {
            ecs_component_record_t *cur = world->cr_non_fragmenting_head;
            while (cur) {
                ecs_component_record_t *next = cur->non_fragmenting.next;
                if (!ecs_id_is_wildcard(cur->id) && cur->sparse &&
                    flecs_component_sparse_has(cur, e) &&
                    !flecs_snapshot_entity_find(se, cur->id))
                {
                    return false;
                }
                cur = next;
            }
        }
    }

    return true;
}

/* Apply the captured state of a single entity. Hierarchy ids are applied in the
 * first pass, everything else in the second pass. */
static void flecs_snapshot_apply_entity(
    ecs_world_t *world,
    ecs_snapshot_entity_t *se)
{
    ecs_entity_t e = se->entity;

    if (!ecs_is_alive(world, e)) {
        ecs_make_alive(world, e);
    }

    /* Remove owned ids that are not in the snapshot. */
    const ecs_type_t *type = ecs_get_type(world, e);
    if (type) {
        int32_t i, count = type->count;
        ecs_vec_t to_remove;
        ecs_vec_init_t(&world->allocator, &to_remove, ecs_id_t, 0);
        for (i = 0; i < count; i ++) {
            ecs_id_t id = type->array[i];
            if (ECS_IS_PAIR(id) && ECS_PAIR_FIRST(id) == EcsParentDepth &&
                ECS_PAIR_SECOND(id) != EcsWildcard)
            {
                continue;
            }
            ecs_id_t cmp = ECS_HAS_ID_FLAG(id, TOGGLE)
                ? (id & ~ECS_TOGGLE) : id;
            if (!flecs_snapshot_entity_find(se, cmp)) {
                ecs_vec_append_t(
                    &world->allocator, &to_remove, ecs_id_t)[0] = id;
            }
        }

        /* Non-fragmenting components are not part of the table type. */
        ecs_record_t *r = flecs_entities_get(world, e);
        if (r && (r->row & EcsEntityHasDontFragment)) {
            ecs_component_record_t *cur = world->cr_non_fragmenting_head;
            while (cur) {
                ecs_component_record_t *next = cur->non_fragmenting.next;
                if (!ecs_id_is_wildcard(cur->id) && cur->sparse &&
                    flecs_component_sparse_has(cur, e) &&
                    !flecs_snapshot_entity_find(se, cur->id))
                {
                    ecs_vec_append_t(
                        &world->allocator, &to_remove, ecs_id_t)[0] = cur->id;
                }
                cur = next;
            }
        }

        count = ecs_vec_count(&to_remove);
        for (i = 0; i < count; i ++) {
            ecs_id_t id = ecs_vec_get_t(&to_remove, ecs_id_t, i)[0];
            ecs_remove_id(world, e, id);
        }
        ecs_vec_fini_t(&world->allocator, &to_remove, ecs_id_t);
    }

    int32_t pass, id_count = ecs_vec_count(&se->ids);
    for (pass = 0; pass < 2; pass ++) {
        int32_t j;
        for (j = 0; j < id_count; j ++) {
            ecs_snapshot_id_t *sid = ecs_vec_get_t(
                &se->ids, ecs_snapshot_id_t, j);

            ecs_id_t id = sid->id;
            bool hierarchy = (id == ecs_id(EcsParent)) ||
                (ECS_IS_PAIR(id) && ECS_PAIR_FIRST(id) == EcsChildOf);

            if ((pass == 0) != hierarchy) {
                continue;
            }

            if (sid->is_toggle) {
                if (!flecs_snapshot_data_eq(world, e, sid)) {
                    ecs_enable_id(world, e, id & ~ECS_TOGGLE, sid->enabled);
                }
                continue;
            }

            if (!sid->size) {
                if (!ecs_has_id(world, e, id)) {
                    ecs_add_id(world, e, id);
                }
                continue;
            }

            if (!flecs_snapshot_data_eq(world, e, sid)) {
                ecs_set_id(world, e, id, flecs_uto(size_t, sid->size),
                    sid->data);
            }
        }
    }
}

ecs_snapshot_result_t ecs_snapshot_restore(
    ecs_world_t *world,
    const ecs_snapshot_t *snapshot_arg)
{
    if (!ecs_snapshot_is_valid(snapshot_arg)) {
        return EcsSnapshotInvalidSnapshot;
    }
    if (!world) {
        return EcsSnapshotInvalidWorld;
    }
    ecs_world_t *w = ECS_CONST_CAST(ecs_world_t*, ecs_get_world(world));
    if (!flecs_poly_is(w, ecs_world_t)) {
        return EcsSnapshotInvalidWorld;
    }
    if (w->flags & (EcsWorldReadonly|EcsWorldMultiThreaded|EcsWorldFini)) {
        return EcsSnapshotInvalidWorld;
    }
    if (w->flush_depth) {
        return EcsSnapshotInvalidOperation;
    }
    if (flecs_snapshot_has_pending_commands(w)) {
        return EcsSnapshotInvalidOperation;
    }

    flecs_check_exclusive_world_access_write(w);

    ecs_snapshot_t *snapshot = ECS_CONST_CAST(ecs_snapshot_t*, snapshot_arg);

    ecs_snapshot_result_t result = flecs_snapshot_validate(w, snapshot);
    if (result != EcsSnapshotOk) {
        ecs_err("ecs_snapshot_restore: %s", ecs_snapshot_result_str(result));
        return result;
    }

    bool empty = ecs_vec_count(&snapshot->entities) == 0;

    if (!empty && flecs_snapshot_matches(w, snapshot)) {
        return EcsSnapshotNoChanges;
    }

    /* Delete user entities that are alive now but not in the snapshot. */
    ecs_entities_t entities = ecs_get_entities(w);
    ecs_vec_t to_delete;
    ecs_vec_init_t(&w->allocator, &to_delete, ecs_entity_t, 0);
    int32_t i;
    for (i = 0; i < entities.alive_count; i ++) {
        ecs_entity_t e = entities.ids[i];
        if (!flecs_snapshot_entity_is_user(w, e)) {
            continue;
        }
        if (!flecs_snapshot_contains(snapshot, e)) {
            ecs_vec_append_t(&w->allocator, &to_delete, ecs_entity_t)[0] = e;
        }
    }
    int32_t del_count = ecs_vec_count(&to_delete);
    for (i = 0; i < del_count; i ++) {
        ecs_delete(w, ecs_vec_get_t(&to_delete, ecs_entity_t, i)[0]);
    }
    ecs_vec_fini_t(&w->allocator, &to_delete, ecs_entity_t);

    /* Make all captured entities alive first, so that hierarchy references
     * resolve regardless of apply order. */
    int32_t s, count = ecs_vec_count(&snapshot->entities);
    for (s = 0; s < count; s ++) {
        ecs_snapshot_entity_t *se = ecs_vec_get_t(
            &snapshot->entities, ecs_snapshot_entity_t, s);
        if (!ecs_is_alive(w, se->entity)) {
            ecs_make_alive(w, se->entity);
        }
    }

    /* Apply captured entities in snapshot (dense) order. */
    for (s = 0; s < count; s ++) {
        ecs_snapshot_entity_t *se = ecs_vec_get_t(
            &snapshot->entities, ecs_snapshot_entity_t, s);
        flecs_snapshot_apply_entity(w, se);
    }

    /* Restore the entity index order, generations and max id so that ids
     * issued after restore are reproduced exactly. Internal entities created
     * after the snapshot (observers/queries/systems) are preserved. */
    flecs_entity_index_restore_dense(ecs_eis(w), snapshot->dense,
        snapshot->dense_count, snapshot->alive_count, snapshot->max_id);

    return empty ? EcsSnapshotEmpty : EcsSnapshotOk;
}

#endif // FLECS_SNAPSHOT
