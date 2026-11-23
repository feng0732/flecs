#include <core.h>
#include "../../../src/private_api.h"

typedef struct { float x, y; } SnapshotPosition;
typedef struct { float x, y; } SnapshotVelocity;

ECS_COMPONENT_DECLARE(SnapshotPosition);
ECS_COMPONENT_DECLARE(SnapshotVelocity);

enum {
    SnapshotProbeAdd,
    SnapshotProbeSet,
    SnapshotProbeRemove
};

typedef struct {
    int32_t add;
    int32_t set;
    int32_t remove;
} SnapshotProbe;

static void SnapshotProbeAdd_cb(ecs_iter_t *it) {
    SnapshotProbe *p = it->ctx;
    p->add += it->count;
}

static void SnapshotProbeSet_cb(ecs_iter_t *it) {
    SnapshotProbe *p = it->ctx;
    p->set += it->count;
}

static void SnapshotProbeRemove_cb(ecs_iter_t *it) {
    SnapshotProbe *p = it->ctx;
    p->remove += it->count;
}

void Snapshot_take_restore_roundtrip(void) {
    ecs_world_t *world = ecs_mini();
    ECS_COMPONENT(world, SnapshotPosition);
    ECS_COMPONENT(world, SnapshotVelocity);

    ecs_entity_t parent = ecs_entity(world, { .name = "parent" });
    ecs_entity_t a = ecs_entity(world, { .name = "a", .parent = parent });
    ecs_set(world, a, SnapshotPosition, {1, 2});
    ecs_set(world, a, SnapshotVelocity, {3, 4});
    ecs_entity_t b = ecs_new_w_parent(world, parent, "b");
    ecs_set(world, b, SnapshotPosition, {5, 6});

    ecs_snapshot_t *snap = ecs_snapshot_take(world);
    test_assert(snap != NULL);
    test_assert(ecs_snapshot_entity_count(snap) >= 3);
    test_assert(ecs_snapshot_id(snap) != 0);

    ecs_set(world, a, SnapshotPosition, {100, 100});
    ecs_delete(world, b);
    ecs_entity_t c = ecs_new(world);
    ecs_set(world, c, SnapshotVelocity, {9, 9});

    ecs_snapshot_result_t r = ecs_snapshot_restore(world, snap);
    test_int(r, EcsSnapshotOk);

    test_assert(ecs_is_alive(world, a));
    test_assert(ecs_is_alive(world, b));
    test_assert(!ecs_is_alive(world, c));

    const SnapshotPosition *pa = ecs_get(world, a, SnapshotPosition);
    test_assert(pa != NULL);
    test_flt(pa->x, 1);
    test_flt(pa->y, 2);
    const SnapshotVelocity *va = ecs_get(world, a, SnapshotVelocity);
    test_assert(va != NULL);
    test_flt(va->x, 3);
    test_flt(va->y, 4);
    const SnapshotPosition *pb = ecs_get(world, b, SnapshotPosition);
    test_assert(pb != NULL);
    test_flt(pb->x, 5);
    test_flt(pb->y, 6);
    test_assert(ecs_get_parent(world, b) == parent);

    char *path = ecs_get_path(world, a);
    test_str(path, "parent.a");
    ecs_os_free(path);

    ecs_snapshot_free(snap);
    ecs_fini(world);
}

void Snapshot_restore_is_idempotent(void) {
    ecs_world_t *world = ecs_mini();
    ECS_COMPONENT(world, SnapshotPosition);

    ecs_entity_t e = ecs_new(world);
    ecs_set(world, e, SnapshotPosition, {1, 2});

    SnapshotProbe probe = {0};
    ecs_observer(world, {
        .query.terms = {{ .id = ecs_id(SnapshotPosition) }},
        .events = { EcsOnAdd, EcsOnSet, EcsOnRemove },
        .callback = SnapshotProbeSet_cb,
        .ctx = &probe
    });
    ecs_observer(world, {
        .query.terms = {{ .id = ecs_id(SnapshotPosition) }},
        .events = { EcsOnAdd },
        .callback = SnapshotProbeAdd_cb,
        .ctx = &probe
    });
    ecs_observer(world, {
        .query.terms = {{ .id = ecs_id(SnapshotPosition) }},
        .events = { EcsOnRemove },
        .callback = SnapshotProbeRemove_cb,
        .ctx = &probe
    });

    ecs_snapshot_t *snap = ecs_snapshot_take(world);

    /* Restoring an identical snapshot must not invoke any lifecycle events. */
    int32_t add = probe.add, set = probe.set, rem = probe.remove;
    ecs_snapshot_result_t r = ecs_snapshot_restore(world, snap);
    test_int(r, EcsSnapshotNoChanges);
    test_int(probe.add, add);
    test_int(probe.set, set);
    test_int(probe.remove, rem);

    ecs_snapshot_free(snap);
    ecs_fini(world);
}

void Snapshot_restore_repeat_after_change(void) {
    ecs_world_t *world = ecs_mini();
    ECS_COMPONENT(world, SnapshotPosition);

    ecs_entity_t e = ecs_new(world);
    ecs_set(world, e, SnapshotPosition, {1, 2});

    ecs_snapshot_t *snap = ecs_snapshot_take(world);

    ecs_set(world, e, SnapshotPosition, {9, 9});

    ecs_snapshot_result_t r = ecs_snapshot_restore(world, snap);
    test_int(r, EcsSnapshotOk);
    const SnapshotPosition *p = ecs_get(world, e, SnapshotPosition);
    test_assert(p != NULL);
    test_flt(p->x, 1);
    test_flt(p->y, 2);

    /* A second restore of the same snapshot is a no-op. */
    r = ecs_snapshot_restore(world, snap);
    test_int(r, EcsSnapshotNoChanges);

    ecs_snapshot_free(snap);
    ecs_fini(world);
}

void Snapshot_empty(void) {
    ecs_world_t *world = ecs_mini();
    ECS_COMPONENT(world, SnapshotPosition);

    ecs_snapshot_t *snap = ecs_snapshot_take(world);
    test_assert(snap != NULL);
    test_int(ecs_snapshot_entity_count(snap), 0);

    ecs_entity_t e = ecs_new(world);
    ecs_set(world, e, SnapshotPosition, {1, 1});

    ecs_snapshot_result_t r = ecs_snapshot_restore(world, snap);
    test_int(r, EcsSnapshotEmpty);
    test_assert(!ecs_is_alive(world, e));

    ecs_snapshot_free(snap);
    ecs_fini(world);
}

void Snapshot_invalid_snapshot(void) {
    ecs_world_t *world = ecs_mini();

    test_int(ecs_snapshot_restore(world, NULL), EcsSnapshotInvalidSnapshot);
    test_assert(!ecs_snapshot_is_valid(NULL));
    test_int(ecs_snapshot_entity_count(NULL), -1);
    test_int(ecs_snapshot_table_count(NULL), -1);
    test_int(ecs_snapshot_id(NULL), 0);

    /* Freeing NULL is a no-op. */
    ecs_snapshot_free(NULL);

    ecs_fini(world);
}

void Snapshot_dangling_snapshot(void) {
    ecs_world_t *world = ecs_mini();
    ECS_COMPONENT(world, SnapshotPosition);

    ecs_entity_t e = ecs_new(world);
    ecs_set(world, e, SnapshotPosition, {1, 1});

    ecs_snapshot_t *snap = ecs_snapshot_take(world);
    test_assert(ecs_snapshot_is_valid(snap));

    ecs_snapshot_free(snap);
    test_assert(!ecs_snapshot_is_valid(snap));

    /* Restoring a freed snapshot must be detected, not crash. */
    test_int(ecs_snapshot_restore(world, snap), EcsSnapshotInvalidSnapshot);

    ecs_fini(world);
}

void Snapshot_restore_recycled_entity(void) {
    ecs_world_t *world = ecs_mini();
    ECS_COMPONENT(world, SnapshotPosition);

    ecs_entity_t e = ecs_new(world);
    ecs_set(world, e, SnapshotPosition, {1, 2});

    ecs_snapshot_t *snap = ecs_snapshot_take(world);

    /* Recycle the id. */
    ecs_delete(world, e);
    ecs_entity_t e2 = ecs_new(world);
    test_assert((uint32_t)e2 == (uint32_t)e);

    /* Restore must bring back the original generation and state. */
    ecs_snapshot_result_t r = ecs_snapshot_restore(world, snap);
    test_int(r, EcsSnapshotOk);
    test_assert(ecs_is_alive(world, e));
    const SnapshotPosition *p = ecs_get(world, e, SnapshotPosition);
    test_assert(p != NULL);
    test_flt(p->x, 1);
    test_flt(p->y, 2);

    ecs_snapshot_free(snap);
    ecs_fini(world);
}

void Snapshot_version_mismatch(void) {
    ecs_world_t *w1 = ecs_mini();
    ECS_COMPONENT(w1, SnapshotPosition);
    ecs_entity_t e = ecs_new(w1);
    ecs_set(w1, e, SnapshotPosition, {1, 1});

    ecs_snapshot_t *snap = ecs_snapshot_take(w1);

    /* World without the component registered has a different schema. */
    ecs_world_t *w2 = ecs_mini();
    ecs_snapshot_result_t r = ecs_snapshot_restore(w2, snap);
    test_int(r, EcsSnapshotVersionMismatch);

    ecs_snapshot_free(snap);
    ecs_fini(w1);
    ecs_fini(w2);
}

void Snapshot_observers_queries_survive(void) {
    ecs_world_t *world = ecs_mini();
    ECS_COMPONENT(world, SnapshotPosition);

    ecs_entity_t e = ecs_new(world);
    ecs_set(world, e, SnapshotPosition, {1, 1});

    ecs_query_t *q = ecs_query(world, {
        .terms = {{ .id = ecs_id(SnapshotPosition) }}
    });
    test_assert(q != NULL);

    SnapshotProbe probe = {0};
    ecs_observer(world, {
        .query.terms = {{ .id = ecs_id(SnapshotPosition) }},
        .events = { EcsOnSet },
        .callback = SnapshotProbeSet_cb,
        .ctx = &probe
    });

    ecs_snapshot_t *snap = ecs_snapshot_take(world);

    ecs_entity_t e2 = ecs_new(world);
    ecs_snapshot_restore(world, snap);
    test_assert(!ecs_is_alive(world, e2));

    /* Query still matches. */
    int32_t count = 0;
    ecs_iter_t it = ecs_query_iter(world, q);
    while (ecs_query_next(&it)) {
        count += it.count;
    }
    test_int(count, 1);

    /* Observer still fires. */
    probe.set = 0;
    ecs_set(world, e, SnapshotPosition, {2, 2});
    test_int(probe.set, 1);

    ecs_snapshot_free(snap);
    ecs_fini(world);
}

void Snapshot_sparse_toggle(void) {
    ecs_world_t *world = ecs_mini();
    ECS_COMPONENT(world, SnapshotPosition);
    ecs_add_id(world, ecs_id(SnapshotPosition), EcsSparse);
    ecs_add_id(world, ecs_id(SnapshotPosition), EcsCanToggle);

    ecs_entity_t e = ecs_new(world);
    ecs_set(world, e, SnapshotPosition, {7, 8});
    ecs_enable_id(world, e, ecs_id(SnapshotPosition), false);
    test_bool(ecs_is_enabled(world, e, SnapshotPosition), false);

    ecs_snapshot_t *snap = ecs_snapshot_take(world);

    ecs_set(world, e, SnapshotPosition, {0, 0});
    ecs_enable_id(world, e, ecs_id(SnapshotPosition), true);
    test_bool(ecs_is_enabled(world, e, SnapshotPosition), true);

    ecs_snapshot_result_t r = ecs_snapshot_restore(world, snap);
    test_int(r, EcsSnapshotOk);

    const SnapshotPosition *p = ecs_get(world, e, SnapshotPosition);
    test_assert(p != NULL);
    test_flt(p->x, 7);
    test_flt(p->y, 8);
    test_bool(ecs_is_enabled(world, e, SnapshotPosition), false);

    r = ecs_snapshot_restore(world, snap);
    test_int(r, EcsSnapshotNoChanges);

    ecs_snapshot_free(snap);
    ecs_fini(world);
}

void Snapshot_deterministic_ids(void) {
    ecs_world_t *world = ecs_mini();

    ecs_entity_t a = ecs_new(world);
    ecs_entity_t b = ecs_new(world);
    (void)a; (void)b;

    ecs_snapshot_t *snap = ecs_snapshot_take(world);

    /* Create ids after the snapshot. */
    ecs_entity_t c = ecs_new(world);
    ecs_entity_t d = ecs_new(world);
    (void)c; (void)d;

    ecs_snapshot_restore(world, snap);

    /* The same sequence of ids must be issued after restore. */
    ecs_entity_t c2 = ecs_new(world);
    ecs_entity_t d2 = ecs_new(world);
    test_uint(c2, c);
    test_uint(d2, d);

    ecs_snapshot_free(snap);
    ecs_fini(world);
}

void Snapshot_dont_fragment(void) {
    ecs_world_t *world = ecs_mini();
    ECS_COMPONENT(world, SnapshotPosition);
    ECS_COMPONENT(world, SnapshotVelocity);
    ecs_add_id(world, ecs_id(SnapshotVelocity), EcsDontFragment);

    ecs_entity_t e = ecs_new(world);
    ecs_set(world, e, SnapshotPosition, {1, 1});
    ecs_set(world, e, SnapshotVelocity, {2, 2});

    /* Non-fragmenting components are not part of the table type. */
    const ecs_type_t *type = ecs_get_type(world, e);
    test_int(type->count, 1);

    ecs_snapshot_t *snap = ecs_snapshot_take(world);

    ecs_set(world, e, SnapshotVelocity, {9, 9});

    ecs_snapshot_result_t r = ecs_snapshot_restore(world, snap);
    test_int(r, EcsSnapshotOk);
    const SnapshotVelocity *v = ecs_get(world, e, SnapshotVelocity);
    test_assert(v != NULL);
    test_flt(v->x, 2);
    test_flt(v->y, 2);

    r = ecs_snapshot_restore(world, snap);
    test_int(r, EcsSnapshotNoChanges);

    ecs_snapshot_free(snap);
    ecs_fini(world);
}

void Snapshot_delete_extra_entities(void) {
    ecs_world_t *world = ecs_mini();
    ECS_COMPONENT(world, SnapshotPosition);

    ecs_entity_t keep = ecs_new(world);
    ecs_set(world, keep, SnapshotPosition, {1, 1});

    ecs_snapshot_t *snap = ecs_snapshot_take(world);

    ecs_entity_t extra = ecs_new(world);
    ecs_set(world, extra, SnapshotPosition, {2, 2});

    ecs_snapshot_result_t r = ecs_snapshot_restore(world, snap);
    test_int(r, EcsSnapshotOk);
    test_assert(ecs_is_alive(world, keep));
    test_assert(!ecs_is_alive(world, extra));

    ecs_snapshot_free(snap);
    ecs_fini(world);
}
