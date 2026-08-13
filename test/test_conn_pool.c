#include "../traffic-gen/core/conn_pool.h"
#include "../traffic-gen/core/flow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_TRUE(condition)                                                 \
        do {                                                                   \
                if (!(condition)) {                                            \
                        fprintf(stderr, "%s:%d assertion failed: %s\n",        \
                                __FILE__, __LINE__, #condition);               \
                        return -1;                                             \
                }                                                              \
        } while (0)

static int test_class_keyed_reuse(void) {
        struct tg_conn_pool pool;
        struct tg_class_plan class_a = {0};
        struct tg_class_plan class_b = {0};
        struct tg_flow flow_a = {0};
        struct tg_flow flow_b = {0};

        ASSERT_TRUE(tg_conn_pool_init(&pool, 2) == 0);
        ASSERT_TRUE(tg_conn_pool_attach(&pool, &flow_a, &class_a) == 0);
        ASSERT_TRUE(tg_conn_pool_attach(&pool, &flow_b, &class_b) == 0);
        ASSERT_TRUE(pool.connections == 2);
        ASSERT_TRUE(tg_conn_pool_put_idle(&pool, &flow_a) == 0);
        ASSERT_TRUE(tg_conn_pool_put_idle(&pool, &flow_b) == 0);
        ASSERT_TRUE(tg_conn_pool_take_idle(&pool, &class_a) == &flow_a);
        ASSERT_TRUE(!flow_a.in_idle_pool);
        ASSERT_TRUE(tg_conn_pool_take_idle(&pool, &class_a) == NULL);
        ASSERT_TRUE(tg_conn_pool_take_any_idle(&pool) == &flow_b);

        tg_conn_pool_detach(&pool, &flow_a);
        tg_conn_pool_detach(&pool, &flow_b);
        ASSERT_TRUE(pool.connections == 0);
        tg_conn_pool_fini(&pool);
        return 0;
}

static int test_capacity_and_drain(void) {
        struct tg_conn_pool pool;
        struct tg_class_plan class_plan = {0};
        struct tg_flow flow_a = {0};
        struct tg_flow flow_b = {0};

        ASSERT_TRUE(tg_conn_pool_init(&pool, 1) == 0);
        ASSERT_TRUE(tg_conn_pool_attach(&pool, &flow_a, &class_plan) == 0);
        ASSERT_TRUE(!tg_conn_pool_can_create(&pool));
        ASSERT_TRUE(tg_conn_pool_attach(&pool, &flow_b, &class_plan) != 0);
        tg_conn_pool_begin_drain(&pool);
        ASSERT_TRUE(!tg_conn_pool_can_create(&pool));
        ASSERT_TRUE(tg_conn_pool_put_idle(&pool, &flow_a) != 0);
        tg_conn_pool_detach(&pool, &flow_a);
        tg_conn_pool_fini(&pool);
        return 0;
}

int main(void) {
        ASSERT_TRUE(test_class_keyed_reuse() == 0);
        ASSERT_TRUE(test_capacity_and_drain() == 0);
        return EXIT_SUCCESS;
}
