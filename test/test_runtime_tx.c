/**
 * @file test_runtime_tx.c
 * @brief Tests worker-owned TX queues, drain budgets, and mbuf reclamation.
 *
 * Run with main lcore 0, worker lcores 1..8, and a net_null device at port 0.
 * A TX callback forces zero, partial, and full acceptance without a physical
 * NIC; each scenario is repeated with 1, 2, 4, and 8 queue owners.
 */

#include "../pro-stack/stack_runtime.h"
#include "../pro-stack/port.h"
#include "../pro-stack/ring.h"
#include "../pro-stack/config.h"
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/** @brief Per-owner ring, runtime counters, and scripted TX acceptance phase. */
struct tx_case {
        struct stack_runtime_worker worker;
        struct inout_ring ring;
        unsigned int phase;
};

/** Verifies queue ownership and limits how many packets reach the null PMD. */
static uint16_t limit_tx(uint16_t port, uint16_t queue,
                         struct rte_mbuf **packets, uint16_t count, void *ctx) {
        struct tx_case *test = ctx;
        (void)packets;
        assert(port == test->worker.port_id);
        assert(queue == test->worker.tx_queue_id);
        assert(rte_lcore_id() == test->worker.lcore_id);
        return test->phase == 0 ? 0 : test->phase == 1 ? count / 2 : count;
}

/** Runs on the owner lcore to check bounded drains and final unsampled drain. */
static int drain_case(void *ctx) {
        struct tx_case *test = ctx;
        struct stack_runtime_worker *worker = &test->worker;
        /* Each budgeted call dequeues one burst, even when TX rejects packets. */
        stack_runtime_tx_drain(worker, 1, true);
        assert(rte_ring_count(test->ring.out) == 4 * BURST_SIZE + 3);
        assert(worker->metrics.tx_nic_drops == BURST_SIZE);
        test->phase = 1;
        stack_runtime_tx_drain(worker, 1, true);
        assert(rte_ring_count(test->ring.out) == 3 * BURST_SIZE + 3);
        /* Finish the three full bursts and short tail without timing samples. */
        test->phase = 2;
        stack_runtime_tx_drain(worker, UINT_MAX, false);
        assert(rte_ring_empty(test->ring.out));
        assert(worker->metrics.tx_nic_drops == BURST_SIZE + BURST_SIZE / 2);
        assert(worker->metrics.tx_packets == 3 * BURST_SIZE + 3 + BURST_SIZE / 2);
        assert(worker->metrics.tx_bursts == 6);
        assert(worker->metrics.nic_tx_sampled_bursts == 2);
        assert(worker->metrics.nic_tx_sampled_packets == BURST_SIZE / 2);
        /* Polling an empty ring must not add a TX burst. */
        stack_runtime_tx_drain(worker, UINT_MAX, false);
        assert(worker->metrics.tx_bursts == 6);
        return 0;
}

/** @brief Exercises queue ownership and full packet reclamation at each scale. */
int main(int argc, char **argv) {
        struct tx_case cases[8] = {0};
        struct rte_eth_conf conf = {0};
        assert(rte_eal_init(argc, argv) >= 0);
        struct rte_mempool *mp = rte_pktmbuf_pool_create("tx_test_mp", 4095, 0, 0,
            RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        assert(mp != NULL);
        assert(rte_eth_dev_configure(0, 1, 8, &conf) == 0);
        assert(rte_eth_rx_queue_setup(0, 0, 128, rte_socket_id(), NULL, mp) == 0);
        for (unsigned int i = 0; i < 8; i++) {
                char name[32];
                assert(rte_eth_tx_queue_setup(0, i, 128, rte_socket_id(), NULL) == 0);
                snprintf(name, sizeof(name), "tx_test_%u", i);
                cases[i].ring.out = rte_ring_create(name, 1024, rte_socket_id(),
                    RING_F_SP_ENQ | RING_F_SC_DEQ);
                assert(cases[i].ring.out != NULL);
                cases[i].worker.ring = &cases[i].ring;
                cases[i].worker.port_id = 0;
                cases[i].worker.tx_queue_id = i;
                cases[i].worker.lcore_id = i + 1;
                cases[i].worker.direct_tx_enabled = true;
                assert(rte_eth_add_tx_callback(0, i, limit_tx, &cases[i]) != NULL);
        }
        assert(rte_eth_dev_start(0) == 0);
        unsigned int baseline = rte_mempool_avail_count(mp);
        for (unsigned int workers = 1; workers <= 8; workers *= 2) {
                /* Direct TX requires at least one dedicated queue per owner. */
                struct port_topology topology = {.worker_count = workers,
                                                  .tx_queue_count = workers};
                assert(port_has_dedicated_worker_tx(&topology));
                topology.tx_queue_count--;
                assert(!port_has_dedicated_worker_tx(&topology));
                for (unsigned int i = 0; i < workers; i++) {
                        memset(&cases[i].worker.metrics, 0, sizeof(cases[i].worker.metrics));
                        cases[i].phase = 0;
                        for (unsigned int p = 0; p < 5 * BURST_SIZE + 3; p++) {
                                struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mp);
                                assert(mbuf != NULL);
                                assert(rte_pktmbuf_append(mbuf, 64) != NULL);
                                assert(rte_ring_sp_enqueue(cases[i].ring.out, mbuf) == 0);
                        }
                        /* The Main lcore cannot consume a worker-owned ring. */
                        stack_runtime_tx_drain(&cases[i].worker, UINT_MAX, false);
                        assert(rte_ring_count(cases[i].ring.out) == 5 * BURST_SIZE + 3);
                        assert(rte_eal_remote_launch(drain_case, &cases[i], i + 1) == 0);
                }
                for (unsigned int i = 0; i < workers; i++)
                        assert(rte_eal_wait_lcore(i + 1) == 0);
                /* Accepted and rejected packets must both return to the pool. */
                assert(rte_mempool_avail_count(mp) == baseline);
        }
        assert(rte_eth_dev_stop(0) == 0);
        assert(rte_eth_dev_close(0) == 0);
        for (unsigned int i = 0; i < 8; i++)
                rte_ring_free(cases[i].ring.out);
        rte_mempool_free(mp);
        assert(rte_eal_cleanup() == 0);
        puts("runtime TX: 1/2/4/8 owners, budget, zero/partial burst, final drain passed");
        return 0;
}
