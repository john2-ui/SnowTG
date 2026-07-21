/**
 * @file log.h
 * @brief Lightweight leveled logging plus IP/MAC formatting helpers.
 *
 * Replaces the ad-hoc `printf("[%s:%s:%d] ...", __FILE__, __func__, __LINE__)`
 * calls that were scattered through the code. Each macro prints a level tag,
 * the source location and the message. Set LOG_LEVEL at compile time (e.g.
 * `-DLOG_LEVEL=LOG_LVL_DEBUG`) to control verbosity.
 */
#ifndef NETARCH_LOG_H
#define NETARCH_LOG_H

#include <stdio.h>
#include <string.h>

enum {
        LOG_LVL_ERROR = 0,
        LOG_LVL_WARN,
        LOG_LVL_INFO,
        LOG_LVL_DEBUG,
};

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LVL_INFO
#endif

/** File name without the leading directory components. */
#define LOG_BASENAME_ (strrchr("/" __FILE__, '/') + 1)

#define NET_LOG_(lvl, tag, fmt, ...)                                           \
        do {                                                                   \
                if ((lvl) <= LOG_LEVEL)                                        \
                        fprintf(stderr, "[%-5s][%s:%d] " fmt "\n", tag,        \
                                LOG_BASENAME_, __LINE__, ##__VA_ARGS__);       \
        } while (0)

#define LOG_ERROR(fmt, ...) NET_LOG_(LOG_LVL_ERROR, "ERROR", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) NET_LOG_(LOG_LVL_WARN, "WARN", fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) NET_LOG_(LOG_LVL_INFO, "INFO", fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) NET_LOG_(LOG_LVL_DEBUG, "DEBUG", fmt, ##__VA_ARGS__)

/**
 * printf helpers for addresses. Using format-string fragments avoids the
 * shared-static-buffer pitfall of inet_ntoa() when printing several addresses
 * in a single call.
 *
 * Example: LOG_INFO("from " IP_FMT ":%u", IP_ARG(ip), rte_be_to_cpu_16(port));
 */
#define IP_FMT "%u.%u.%u.%u"
#define IP_ARG(be_ip)                                                          \
        ((be_ip) & 0xff), (((be_ip) >> 8) & 0xff), (((be_ip) >> 16) & 0xff),   \
            (((be_ip) >> 24) & 0xff)

#define MAC_FMT "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC_ARG(m) (m)[0], (m)[1], (m)[2], (m)[3], (m)[4], (m)[5]

#endif /* NETARCH_LOG_H */
