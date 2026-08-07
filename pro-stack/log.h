/**
 * @file log.h
 * @brief Lightweight leveled logging plus IP/MAC formatting helpers.
 *
 * Replaces the ad-hoc `printf("[%s:%s:%d] ...", __FILE__, __func__, __LINE__)`
 * calls that were scattered through the code. Each macro prints a module and
 * level tag, the source location and the message. Set LOG_LEVEL at compile
 * time (e.g. `-DLOG_LEVEL=LOG_LVL_DEBUG`) to control verbosity.
 */
#ifndef NETARCH_LOG_H
#define NETARCH_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
        LOG_LVL_ERROR = 0,
        LOG_LVL_WARN,
        LOG_LVL_INFO,
        LOG_LVL_DEBUG,
        LOG_LVL_TRACE,
};

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LVL_INFO
#endif

/** File name without the leading directory components. */
#define LOG_BASENAME_ (strrchr("/" __FILE__, '/') + 1)

/**
 * Return whether log tags should use ANSI color escapes.
 *
 * LOG_COLOR=always, auto (the default), or never selects the behavior.
 * NO_COLOR always disables color so redirected output and log collectors stay
 * machine-readable by default.
 */
static inline int net_log_color_enabled(void) {
        const char *mode = getenv("LOG_COLOR");

        if (getenv("NO_COLOR") != NULL)
                return 0;
        if (mode != NULL && strcmp(mode, "never") == 0)
                return 0;
        if (mode != NULL && strcmp(mode, "always") == 0)
                return 1;
        return isatty(fileno(stderr));
}

static inline const char *net_log_color(unsigned int level) {
        switch (level) {
        case LOG_LVL_ERROR:
                return "\033[1;31m"; /* bold red */
        case LOG_LVL_WARN:
                return "\033[1;33m"; /* bold yellow */
        case LOG_LVL_INFO:
                return "\033[1;32m"; /* bold green */
        case LOG_LVL_DEBUG:
                return "\033[1;36m"; /* bold cyan */
        default:
                return "\033[0;37m"; /* gray trace */
        }
}

/** Bright magenta is reserved for structured field names in log messages. */
#define NET_LOG_KEY_COLOR "\033[1;35m"
#define NET_LOG_COLOR_RESET "\033[0m"

static inline size_t net_log_key_len(const char *text) {
        static const char *const keys[] = {
            "sock=",   "gen=",   "fd=",          "state=",  "local=",
            "peer=",   "event=", "seq=",         "ack=",    "win=",
            "reason=", "una=",   "nxt=",         "sndbuf=", "rcvbuf_used=",
            "wire=",   "scale=", "wnd=",         "retry=",  "rto_ms=",
            "kind=",   "len=",   "result=",      "errno=",  "op=",
            "from=",   "to=",    "owner_lcore=",
        };

        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
                size_t len = strlen(keys[i]);
                if (strncmp(text, keys[i], len) == 0)
                        return len;
        }
        return 0;
}

static inline void net_log_print_message(const char *message, int color) {
        const char *p = message;

        while (*p != '\0') {
                size_t key_len = 0;
                if (p == message || p[-1] == ' ' || p[-1] == '\t')
                        key_len = net_log_key_len(p);
                if (color && key_len > 0) {
                        fprintf(stderr,
                                NET_LOG_KEY_COLOR "%.*s" NET_LOG_COLOR_RESET,
                                (int)key_len, p);
                        p += key_len;
                        continue;
                }
                fputc(*p++, stderr);
        }
}

static inline void net_log_emit(const char *module, unsigned int level,
                                const char *tag, const char *file, int line,
                                const char *fmt, ...) {
        char message[2048];
        va_list args;
        int color = net_log_color_enabled();

        va_start(args, fmt);
        (void)vsnprintf(message, sizeof(message), fmt, args);
        va_end(args);

        if (color)
                fprintf(stderr, "%s[%s][%-5s]" NET_LOG_COLOR_RESET "[%s:%d] ",
                        net_log_color(level), module, tag, file, line);
        else
                fprintf(stderr, "[%s][%-5s][%s:%d] ", module, tag, file, line);
        net_log_print_message(message, color);
        fputc('\n', stderr);
}

#define NET_LOG_MOD_(module, lvl, tag, fmt, ...)                               \
        do {                                                                   \
                if ((lvl) <= LOG_LEVEL)                                        \
                        net_log_emit(module, lvl, tag, LOG_BASENAME_,          \
                                     __LINE__, fmt, ##__VA_ARGS__);            \
        } while (0)

#define LOG_MOD_ERROR(module, fmt, ...)                                        \
        NET_LOG_MOD_(module, LOG_LVL_ERROR, "ERROR", fmt, ##__VA_ARGS__)
#define LOG_MOD_WARN(module, fmt, ...)                                         \
        NET_LOG_MOD_(module, LOG_LVL_WARN, "WARN", fmt, ##__VA_ARGS__)
#define LOG_MOD_INFO(module, fmt, ...)                                         \
        NET_LOG_MOD_(module, LOG_LVL_INFO, "INFO", fmt, ##__VA_ARGS__)
#define LOG_MOD_DEBUG(module, fmt, ...)                                        \
        NET_LOG_MOD_(module, LOG_LVL_DEBUG, "DEBUG", fmt, ##__VA_ARGS__)
#define LOG_MOD_TRACE(module, fmt, ...)                                        \
        NET_LOG_MOD_(module, LOG_LVL_TRACE, "TRACE", fmt, ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) LOG_MOD_ERROR("CORE", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) LOG_MOD_WARN("CORE", fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) LOG_MOD_INFO("CORE", fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) LOG_MOD_DEBUG("CORE", fmt, ##__VA_ARGS__)
#define LOG_TRACE(fmt, ...) LOG_MOD_TRACE("CORE", fmt, ##__VA_ARGS__)

#define LOG_TCP_ERROR(fmt, ...) LOG_MOD_ERROR("TCP", fmt, ##__VA_ARGS__)
#define LOG_TCP_WARN(fmt, ...) LOG_MOD_WARN("TCP", fmt, ##__VA_ARGS__)

/**
 * ARP learning and request/reply messages are expected on normal traffic
 * paths. Keep them disabled unless ARP diagnosis is explicitly requested.
 */
#ifndef ARP_LOG_ENABLED
#define ARP_LOG_ENABLED 0
#endif

#if ARP_LOG_ENABLED
#define LOG_ARP_INFO(fmt, ...) LOG_MOD_INFO("ARP", fmt, ##__VA_ARGS__)
#define LOG_ARP_DEBUG(fmt, ...) LOG_MOD_DEBUG("ARP", fmt, ##__VA_ARGS__)
#else
#define LOG_ARP_INFO(fmt, ...)                                                 \
        do {                                                                   \
                if (0)                                                         \
                        LOG_MOD_INFO("ARP", fmt, ##__VA_ARGS__);               \
        } while (0)
#define LOG_ARP_DEBUG(fmt, ...)                                                \
        do {                                                                   \
                if (0)                                                         \
                        LOG_MOD_DEBUG("ARP", fmt, ##__VA_ARGS__);              \
        } while (0)
#endif

/**
 * TCP state-transition and packet-path messages are verbose during traffic
 * generation.  Keep TCP errors and warnings enabled while compiling INFO,
 * DEBUG, and TRACE calls out by default.  Define the needed option as 1 in
 * CFLAGS when diagnosing TCP behavior.
 */
#ifndef TCP_LOG_INFO_ENABLED
#define TCP_LOG_INFO_ENABLED 0
#endif

#ifndef TCP_LOG_TRACE_ENABLED
#define TCP_LOG_TRACE_ENABLED 0
#endif

#ifndef TCP_LOG_DEBUG_ENABLED
#define TCP_LOG_DEBUG_ENABLED 0
#endif

#if TCP_LOG_INFO_ENABLED
#define LOG_TCP_INFO(fmt, ...) LOG_MOD_INFO("TCP", fmt, ##__VA_ARGS__)
#else
#define LOG_TCP_INFO(fmt, ...)                                                 \
        do {                                                                   \
                if (0)                                                         \
                        LOG_MOD_INFO("TCP", fmt, ##__VA_ARGS__);               \
        } while (0)
#endif

#if TCP_LOG_DEBUG_ENABLED
#define LOG_TCP_DEBUG(fmt, ...) LOG_MOD_DEBUG("TCP", fmt, ##__VA_ARGS__)
#else
#define LOG_TCP_DEBUG(fmt, ...)                                                \
        do {                                                                   \
                if (0)                                                         \
                        LOG_MOD_DEBUG("TCP", fmt, ##__VA_ARGS__);              \
        } while (0)
#endif

#if TCP_LOG_TRACE_ENABLED
#define LOG_TCP_TRACE(fmt, ...) LOG_MOD_TRACE("TCP", fmt, ##__VA_ARGS__)
#else
#define LOG_TCP_TRACE(fmt, ...)                                                \
        do {                                                                   \
                if (0)                                                         \
                        LOG_MOD_TRACE("TCP", fmt, ##__VA_ARGS__);              \
        } while (0)
#endif

#define LOG_OWNER_ERROR(fmt, ...) LOG_MOD_ERROR("OWNER", fmt, ##__VA_ARGS__)
#define LOG_OWNER_WARN(fmt, ...) LOG_MOD_WARN("OWNER", fmt, ##__VA_ARGS__)
#define LOG_OWNER_INFO(fmt, ...) LOG_MOD_INFO("OWNER", fmt, ##__VA_ARGS__)
#define LOG_OWNER_DEBUG(fmt, ...) LOG_MOD_DEBUG("OWNER", fmt, ##__VA_ARGS__)

#ifndef TCP_LOG_PACKETS
#define TCP_LOG_PACKETS 0
#endif

#if TCP_LOG_PACKETS
#define LOG_TCP_PACKET(fmt, ...) LOG_TCP_TRACE(fmt, ##__VA_ARGS__)
#else
#define LOG_TCP_PACKET(fmt, ...)                                               \
        do {                                                                   \
        } while (0)
#endif

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
