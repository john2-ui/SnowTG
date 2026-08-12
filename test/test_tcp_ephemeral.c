#ifndef TCP_TESTING
#define TCP_TESTING
#endif

#include "../pro-stack/tcp.h"

#include <stdint.h>
#include <stdio.h>

#define CHECK(condition)                                                       \
        do {                                                                   \
                if (!(condition)) {                                            \
                        fprintf(stderr, "check failed: %s (%s:%d)\n",          \
                                #condition, __FILE__, __LINE__);               \
                        return 1;                                              \
                }                                                              \
        } while (0)

int main(void) {
        /*
         * Single-queue mode has no RSS prediction. The negative result must
         * mean "no owner constraint", not "reject every ephemeral port".
         */
        CHECK(tcp_test_port_prediction_matches_owner(-1, 0));
        CHECK(tcp_test_port_prediction_matches_owner(-1, 7));
        CHECK(tcp_test_port_prediction_matches_owner(0, 0));
        CHECK(!tcp_test_port_prediction_matches_owner(1, 0));
        CHECK(tcp_test_port_prediction_matches_owner(1, 1));
        return 0;
}
