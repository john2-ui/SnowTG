/**
 * @file workflow.c
 * @brief Executes bounded business transactions on their owning workers.
 *
 * A business spans multiple protocol exchanges but holds one scheduler slot.
 * Flow and timer callbacks only copy results or enqueue readiness; the budgeted
 * tick advances steps after the prior Flow callback and lifecycle work return.
 */
#include "workflow.h"
#include "../proto/dns/dns_client.h"
#include "../proto/http/http_client.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <rte_cycles.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/**
 * @brief Owned scalar copy independent of configuration and response storage.
 */
struct tg_context_value {
        char name[64];
        struct tg_value value;
};

/**
 * @brief One admitted transaction retained until a single terminal completion.
 *
 * next is shared by the free list and ready queue, never both simultaneously.
 * visited/skipped distinguish executed steps, bypassed paths, and work never
 * reached after failure. The generation-bearing socket handle replaces a stored
 * Flow pointer, which could become stale during close or pool reuse.
 *
 * The request buffer and dynamic protocol configs remain valid until the current
 * exchange finishes. timeout spans the business; wake schedules one think step.
 */
struct tg_business {
        struct tg_business *next;
        struct tg_workflow_engine *engine;
        const struct tg_class_plan *class_plan;
        const struct tg_workflow_plan *plan;
        struct tg_context_value context[TG_WF_VARS];
        unsigned values, step, cls, phase;
        uint32_t visited, skipped;
        uint64_t ordinal, planned, start, step_start, bytes_rx, bytes_tx;
        struct owner_timer timeout, wake;
        struct nsock_handle handle;
        bool active, queued, waiting, thinking, timed_out, failed;
        enum tg_flow_result failure;
        struct tg_http_config http;
        struct tg_dns_config dns;
        char request[TG_WF_REQUEST_CAP];
};

/**
 * @brief Queues owner work without recursively executing another step.
 *
 * queued coalesces response/timer notifications; inactive objects are ignored.
 */
static void enqueue(struct tg_business *b) {
        if (!b->active || b->queued)
                return;
        struct tg_workflow_engine *e = b->engine;
        b->queued = true;
        b->next = NULL;
        if (e->tail)
                e->tail->next = b;
        else
                e->head = b;
        e->tail = b;
}

/**
 * @brief Marks the business deadline and defers cleanup to the owner tick.
 *
 * Closing a Flow here could invoke its completion callback recursively.
 */
static void timeout_cb(struct owner_timer *t, void *arg, uint64_t now) {
        (void)t;
        (void)now;
        struct tg_business *b = arg;
        b->timed_out = true;
        enqueue(b);
}

/**
 * @brief Makes a thinking transaction ready without consuming step budget inline.
 */
static void wake_cb(struct owner_timer *t, void *arg, uint64_t now) {
        (void)t;
        (void)now;
        enqueue(arg);
}

/**
 * @brief Locates step counters using the business admission phase and class.
 *
 * Cross-phase completions remain attributed to the phase that admitted them.
 */
static struct tg_step_stats *step_stats(struct tg_business *b, unsigned step) {
        struct tg_workflow_engine *e = b->engine;
        return &e->steps[b->phase * e->step_count + e->offsets[b->cls] + step];
}

/**
 * @brief Builds a statistics-only Flow snapshot for the complete business.
 *
 * The transaction protocol tag prevents counting the business itself as another
 * HTTP exchange. The snapshot never enters the socket map or connection pool.
 */
static void timing(struct tg_business *b, struct tg_flow *f) {
        memset(f, 0, sizeof(*f));
        f->start_cycles = b->start;
        f->planned_cycles = b->planned;
        f->class_index = b->cls;
        f->load_phase_index = b->phase;
        f->txn.proto = &tg_workflow_proto_ops;
        f->txn.request_offset = b->bytes_tx;
        f->txn.response_bytes = b->bytes_rx;
}

/**
 * @brief Completes the business exactly once and returns its concurrency slot.
 *
 * Only tick calls this after dequeuing the object. Marking inactive before
 * closing an outstanding Flow makes synchronous completion callbacks harmless.
 * Both timers are cancelled before the object returns to the free list.
 */
static void finish(struct tg_business *b, bool ok) {
        struct tg_workflow_engine *e = b->engine;
        b->active = false;
        owner_timer_cancel(&b->timeout);
        owner_timer_cancel(&b->wake);
        if (b->waiting) {
                struct tg_flow *f = tg_flow_map_lookup(e->map, b->handle);
                if (f)
                        tg_flow_close_connection(e->map, e->flows, f, false,
                                                 TG_FLOW_RESULT_IO_FAILURE);
        }
        for (unsigned i = 0; i < b->plan->count; i++)
                if (!((b->visited | b->skipped) & (1U << i)))
                        step_stats(b, i)->counts.not_reached++;
        struct tg_flow f;
        timing(b, &f);
        enum tg_flow_result result = ok ? TG_FLOW_RESULT_SUCCESS : b->failure;
        tg_stats_on_flow_finished(e->stats, &f, result);
        if (e->latency->groups)
                tg_latency_on_finished(e->latency, &f, result,
                                       rte_get_timer_cycles());
        tg_scheduler_on_flow_finished(e->scheduler);
        b->next = e->free;
        e->free = b;
}

/**
 * @brief Copies a scalar from vars, the selected data row, or private ctx.
 *
 * Dataset selection uses the global planned ordinal, including skipped arrivals,
 * not a worker-local count of successful starts. Missing values return -1.
 */
static int lookup(struct tg_business *b, const char *ref,
                  struct tg_value *out) {
        int o = -1;
        const char *key = strchr(ref, '.');
        if (!key || !key[1])
                return -1;
        key++;
        if (!strncmp(ref, "ctx.", 4)) {
                for (unsigned i = 0; i < b->values; i++)
                        if (!strcmp(key, b->context[i].name)) {
                                *out = b->context[i].value;
                                return 0;
                        }
                return -1;
        }
        if (!strncmp(ref, "vars.", 5))
                o = b->plan->vars;
        else if (!strncmp(ref, "data.", 5) && b->plan->row_count)
                o = b->plan->rows[b->ordinal % b->plan->row_count];
        return tg_value_read(&b->plan->doc.view, tg_wf_member(b->plan, o, key),
                             out);
}

/**
 * @brief Copies an extraction into a fixed context slot, replacing an existing key.
 *
 * The compiler bounds the union of extraction keys; runtime checks still reject
 * capacity or name overflow rather than truncating context.
 */
static int save(struct tg_business *b, const char *name,
                const struct tg_value *v) {
        unsigned i = 0;
        for (; i < b->values; i++)
                if (!strcmp(b->context[i].name, name))
                        break;
        if (i == TG_WF_VARS || strlen(name) >= sizeof(b->context[i].name))
                return -1;
        if (i == b->values)
                b->values++;
        strcpy(b->context[i].name, name);
        b->context[i].value = *v;
        return 0;
}

/**
 * @brief Appends bytes while reserving a terminator and rejecting overflow.
 */
static int append(char *out, size_t cap, size_t *n, const char *s, size_t len) {
        if (len >= cap - *n)
                return -1;
        memcpy(out + *n, s, len);
        *n += len;
        out[*n] = 0;
        return 0;
}

/**
 * @brief Expands namespaced variables with optional url or json escaping.
 *
 * The json filter emits a quoted JSON string, suitable as a complete body value.
 * Missing variables, unknown filters, and oversized output fail explicitly; no
 * expression evaluation or runtime script callback occurs here.
 */
static int render(struct tg_business *b, const char *src, char *out,
                  size_t cap) {
        size_t n = 0;
        out[0] = 0;
        while (*src) {
                if (src[0] != '$' || src[1] != '{') {
                        if (append(out, cap, &n, src++, 1))
                                return -1;
                        continue;
                }
                const char *end = strchr(src + 2, '}');
                char ref[128];
                if (!end || end - src - 2 >= (int)sizeof(ref))
                        return -1;
                memcpy(ref, src + 2, (size_t)(end - src - 2));
                ref[end - src - 2] = 0;
                char *filter = strchr(ref, '|');
                if (filter)
                        *filter++ = 0;
                struct tg_value v;
                if (lookup(b, ref, &v))
                        return -1;
                if (filter && strcmp(filter, "url") && strcmp(filter, "json"))
                        return -1;
                if (filter && !strcmp(filter, "json") &&
                    append(out, cap, &n, "\"", 1))
                        return -1;
                for (const unsigned char *p = (const unsigned char *)v.text; *p;
                     p++) {
                        char encoded[8];
                        size_t bytes = 1;
                        encoded[0] = (char)*p;
                        if (filter && !strcmp(filter, "url") &&
                            !(isalnum(*p) || strchr("-._~", *p))) {
                                snprintf(encoded, sizeof(encoded), "%%%02X",
                                         *p);
                                bytes = 3;
                        } else if (filter && !strcmp(filter, "json")) {
                                if (*p < 32) {
                                        snprintf(encoded, sizeof(encoded),
                                                 "\\u%04x", *p);
                                        bytes = 6;
                                } else if (*p == '"' || *p == '\\') {
                                        encoded[0] = '\\';
                                        encoded[1] = (char)*p;
                                        bytes = 2;
                                }
                        }
                        if (append(out, cap, &n, encoded, bytes))
                                return -1;
                }
                if (filter && !strcmp(filter, "json") &&
                    append(out, cap, &n, "\"", 1))
                        return -1;
                src = end + 1;
        }
        return 0;
}

/**
 * @brief Decodes one template field before rendering against the current business.
 *
 * Both source and expanded lengths are bounded. Only absent fields use fallback.
 */
static int field(struct tg_business *b, int object, const char *name,
                 const char *fallback, char *out, size_t cap) {
        char src[TG_WF_REQUEST_CAP];
        return tg_wf_text(b->plan, object, name, fallback, src, sizeof(src)) ||
                       render(b, src, out, cap)
                   ? -1
                   : 0;
}

/**
 * @brief Reads a context reference or asks the active protocol to export a scalar.
 *
 * Response exports must finish before txn reset; branch operands have no txn.
 */
static int operand(struct tg_business *b, int object, const struct tg_txn *txn,
                   struct tg_value *v) {
        char source[32], path[TG_VALUE_CAP], ref[128];
        unsigned index;
        if (tg_wf_member(b->plan, object, "ref") >= 0)
                return tg_wf_text(b->plan, object, "ref", NULL, ref,
                                  sizeof(ref)) ||
                               lookup(b, ref, v)
                           ? -1
                           : 0;
        if (!txn || !txn->proto->export_value ||
            tg_wf_text(b->plan, object, "source", NULL, source,
                       sizeof(source)) ||
            tg_wf_text(b->plan, object, "path", "", path, sizeof(path)) ||
            tg_wf_u32(b->plan, object, "index", 0, 0, 65535, &index))
                return -1;
        return txn->proto->export_value(txn, source, path, index, v);
}

/**
 * @brief Evaluates a typed check or branch condition.
 *
 * exists treats lookup failure as absence. Other operators fail on missing values
 * or type mismatch rather than silently coercing types or selecting the else path.
 */
static int predicate(struct tg_business *b, int object,
                     const struct tg_txn *txn, bool *out) {
        char op[16];
        struct tg_value left, right;
        if (tg_wf_text(b->plan, object, "op", NULL, op, sizeof(op)))
                return -1;
        int rc = operand(b, tg_wf_member(b->plan, object, "left"), txn, &left);
        if (!strcmp(op, "exists")) {
                *out = rc == 0;
                return 0;
        }
        return rc || tg_value_read(&b->plan->doc.view,
                                   tg_wf_member(b->plan, object, "right"),
                                   &right)
                   ? -1
                   : tg_value_compare(&left, op, &right, out);
}

/**
 * @brief Records one terminal step outcome and its optional latency samples.
 *
 * Internal failed includes start_failed; the CSV layer splits them later.
 * Timing begins before network preparation, and think/branch samples remain in
 * their own step histograms rather than inflating network request latency.
 */
static void record_step(struct tg_business *b, bool ok, bool start_failed) {
        struct tg_step_stats *s = step_stats(b, b->step);
        if (ok)
                s->counts.success++;
        else
                s->counts.failed++;
        if (start_failed)
                s->counts.start_failed++;
        if (s->hist) {
                uint64_t us =
                    (uint64_t)(((__uint128_t)(rte_get_timer_cycles() -
                                              b->step_start) *
                                    1000000 +
                                b->engine->scheduler->cycles_per_second - 1) /
                               b->engine->scheduler->cycles_per_second);
                tg_histogram_record(&s->hist[0], us);
                tg_histogram_record(&s->hist[ok ? 1 : 2], us);
        }
}

/**
 * @brief Marks bypassed forward steps and selects the next instruction.
 *
 * Both branch selection and explicit next jumps skip work without request errors.
 */
static void advance(struct tg_business *b, unsigned next) {
        for (unsigned i = b->step + 1; i < next; i++) {
                step_stats(b, i)->counts.branch_skipped++;
                b->skipped |= 1U << i;
        }
        b->step = next;
}

/**
 * @brief Validates and copies results while the Flow parser is still alive.
 *
 * waiting guards duplicate completion. Checks run before extraction, so newly
 * extracted context is available to subsequent steps. HTTP defaults to 2xx unless
 * a status condition explicitly controls acceptance.
 *
 * The callback only enqueues the business; its caller must finish closing or
 * pooling this Flow before tick prepares another request using the same object.
 */
static void response(void *arg, const struct tg_flow *f,
                     enum tg_flow_result result) {
        struct tg_business *b = arg;
        if (!b->active || !b->waiting)
                return;
        b->waiting = false;
        b->bytes_tx += f->txn.request_offset;
        b->bytes_rx += f->txn.response_bytes;
        const struct tg_step_plan *step = &b->plan->steps[b->step];
        const struct tg_json_doc *d = &b->plan->doc.view;
        if (rte_get_timer_cycles() - b->step_start >=
            (uint64_t)step->timeout_ms *
                b->engine->scheduler->cycles_per_second / 1000)
                result = TG_FLOW_RESULT_IO_FAILURE;
        bool ok = result == TG_FLOW_RESULT_SUCCESS;
        bool explicit_status = false;
        if (ok && step->checks >= 0) {
                for (int i = step->checks + 1;
                     i < d->token_count &&
                     d->tokens[i].start < d->tokens[step->checks].end;
                     i = tg_json_next(d, i)) {
                        char source[32];
                        int left = tg_wf_member(b->plan, i, "left");
                        if (!tg_wf_text(b->plan, left, "source", "", source,
                                        sizeof(source)) &&
                            !strcmp(source, "status"))
                                explicit_status = true;
                        bool match = false;
                        if (predicate(b, i, &f->txn, &match) || !match)
                                ok = false;
                }
        }
        if (ok && step->kind == TG_STEP_HTTP && !explicit_status) {
                struct tg_value v;
                if (f->txn.proto->export_value(&f->txn, "status", "", 0, &v) ||
                    atoi(v.text) < 200 || atoi(v.text) >= 300)
                        ok = false;
        }
        if (ok && step->extract >= 0) {
                for (int i = step->extract + 1;
                     i < d->token_count &&
                     d->tokens[i].start < d->tokens[step->extract].end;
                     i = tg_json_next(d, i + 1)) {
                        char name[64];
                        struct tg_value v;
                        if (tg_string_read(d, i, name, sizeof(name)) ||
                            operand(b, i + 1, &f->txn, &v) ||
                            save(b, name, &v)) {
                                ok = false;
                                break;
                        }
                }
        }
        record_step(b, ok, false);
        if (!ok) {
                b->failed = true;
                b->failure = result == TG_FLOW_RESULT_SUCCESS
                                 ? TG_FLOW_RESULT_PROTOCOL_FAILURE
                                 : result;
        } else {
                if (step->kind == TG_STEP_HTTP)
                        b->engine->stats->http_rps_total++;
                advance(b, step->next);
        }
        enqueue(b);
}
static bool safe_text(const char *s, bool token) {
        if (!*s)
                return false;
        for (const unsigned char *p = (const unsigned char *)s; *p; p++)
                if (*p <= 32 || *p == 127 ||
                    (token && !isalnum(*p) && !strchr("!#$%&'*+-.^_`|~", *p)))
                        return false;
        return true;
}
/* HTTP field values allow HTAB and visible/extended bytes, never other CTLs. */
static bool safe_header_value(const char *s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++)
                if ((*p < 32 && *p != '\t') || *p == 127)
                        return false;
        return true;
}

/**
 * @brief Prepares a bounded request and reuses or creates its physical Flow.
 *
 * The business layer validates dynamic targets and fields; plugins serialize
 * protocol framing and Content-Length. Idle reuse must match the actual endpoint
 * and Host. Success sets waiting and the generation-bearing handle; failure is
 * reported by tick as a step start failure.
 */
static int network(struct tg_business *b) {
        /* Template/field validation failures must not inherit socket errno. */
        errno = EINVAL;
        struct tg_workflow_engine *e = b->engine;
        const struct tg_step_plan *s = &b->plan->steps[b->step];
        int peer = tg_wf_member(b->plan, s->object, "peer"),
            cfg = tg_wf_member(b->plan, s->object,
                               s->kind == TG_STEP_HTTP ? "http" : "dns");
        char ip[INET_ADDRSTRLEN];
        unsigned port;
        struct sockaddr_in address = {.sin_family = AF_INET};
        if (field(b, peer, "ip", NULL, ip, sizeof(ip)) ||
            inet_pton(AF_INET, ip, &address.sin_addr) != 1 ||
            tg_wf_u32(b->plan, peer, "port", s->kind == TG_STEP_HTTP ? 80 : 53,
                      1, 65535, &port))
                return -1;
        address.sin_port = htons((uint16_t)port);
        size_t length = 0;
        const struct tg_proto_ops *proto;
        void *config;
        struct tg_flow *flow = NULL;
        if (s->kind == TG_STEP_HTTP) {
                memset(&b->http, 0, sizeof(b->http));
                if (field(b, cfg, "method", "GET", b->http.method,
                          sizeof(b->http.method)) ||
                    !safe_text(b->http.method, true) ||
                    field(b, cfg, "path", "/", b->http.path,
                          sizeof(b->http.path)) ||
                    b->http.path[0] != '/' || !safe_text(b->http.path, false) ||
                    field(b, cfg, "host", ip, b->http.host,
                          sizeof(b->http.host)) ||
                    !safe_text(b->http.host, false))
                        return -1;
                bool keep = false;
                int k = tg_wf_member(b->plan, cfg, "keepalive");
                if (k >= 0 &&
                    tg_json_parse_bool(&b->plan->doc.view,
                                       &b->plan->doc.tokens[k], &keep))
                        return -1;
                b->http.connection_close = !keep;
                b->http.accept_any_status = true;
                /* Selector decoding and capture decisions are startup work;
                 * unrelated request fields named "source" cannot enable it. */
                b->http.capture_headers = s->capture_headers;
                b->http.capture_json = s->capture_json;
                const struct tg_json_doc *d = &b->plan->doc.view;
                char headers[TG_WF_REQUEST_CAP] = "", body[TG_WF_REQUEST_CAP];
                size_t used = 0;
                int h = tg_wf_member(b->plan, cfg, "headers");
                if (h >= 0) {
                        if (d->tokens[h].type != JSMN_OBJECT)
                                return -1;
                        for (int t = h + 1;
                             t < d->token_count &&
                             d->tokens[t].start < d->tokens[h].end;
                             t = tg_json_next(d, t + 1)) {
                                char key[128], raw[TG_WF_REQUEST_CAP],
                                    value[TG_WF_REQUEST_CAP];
                                if (tg_string_read(d, t, key, sizeof(key)) ||
                                    !safe_text(key, true) ||
                                    !strcasecmp(key, "Host") ||
                                    !strcasecmp(key, "Connection") ||
                                    !strcasecmp(key, "Content-Length") ||
                                    !strcasecmp(key, "Transfer-Encoding") ||
                                    !strcasecmp(key, "Upgrade") ||
                                    !strcasecmp(key, "Expect") ||
                                    !strcasecmp(key, "Trailer") ||
                                    !strcasecmp(key, "TE") ||
                                    !strcasecmp(key, "Keep-Alive") ||
                                    !strcasecmp(key, "Proxy-Connection") ||
                                    tg_string_read(d, t + 1, raw,
                                                   sizeof(raw)) ||
                                    render(b, raw, value, sizeof(value)) ||
                                    !safe_header_value(value) ||
                                    append(headers, sizeof(headers), &used, key,
                                           strlen(key)) ||
                                    append(headers, sizeof(headers), &used,
                                           ": ", 2) ||
                                    append(headers, sizeof(headers), &used,
                                           value, strlen(value)) ||
                                    append(headers, sizeof(headers), &used,
                                           "\r\n", 2))
                                        return -1;
                        }
                }
                if (field(b, cfg, "body", "", body, sizeof(body)) ||
                    tg_http_build_parts(&b->http, headers, body,
                                        (uint8_t *)b->request,
                                        sizeof(b->request), &length))
                        return -1;
                proto = &tg_http_proto_ops;
                config = &b->http;
                if (keep)
                        flow = tg_conn_pool_take_matching(
                            e->connections, b->class_plan, &address,
                            b->http.host);
                if (flow) {
                        flow->on_finish = response;
                        flow->on_finish_ctx = b;
                        if (tg_flow_rearm_tcp(flow, proto, config,
                                              (uint8_t *)b->request, length)) {
                                tg_flow_close_connection(
                                    e->map, e->flows, flow, false,
                                    TG_FLOW_RESULT_IO_FAILURE);
                                return -1;
                        }
                        tg_stats_on_connection_reused(e->stats);
                }
        } else {
                memset(&b->dns, 0, sizeof(b->dns));
                b->dns.qtype = TG_DNS_QTYPE_A;
                b->dns.transaction_id = TG_DNS_TRANSACTION_ID;
                b->dns.require_address = true;
                if (field(b, cfg, "qname", NULL, b->dns.qname,
                          sizeof(b->dns.qname)) ||
                    tg_dns_qname_validate(b->dns.qname))
                        return -1;
                proto = &tg_dns_proto_ops;
                config = &b->dns;
                if (proto->build_request(config, (uint8_t *)b->request,
                                         sizeof(b->request), &length))
                        return -1;
        }
        if (!flow) {
                /* Idle connections must not starve a new destination or a UDP
                 * step. */
                if (!e->flows->free_count ||
                    (s->kind == TG_STEP_HTTP &&
                     !tg_conn_pool_can_create(e->connections))) {
                        struct tg_flow *idle =
                            tg_conn_pool_take_any_idle(e->connections);
                        if (idle)
                                tg_flow_close_connection(
                                    e->map, e->flows, idle, false,
                                    TG_FLOW_RESULT_IO_FAILURE);
                }
                int rc;
                if (s->kind == TG_STEP_HTTP)
                        rc = tg_flow_start_tcp(
                            e->map, e->flows, (struct sockaddr *)&address,
                            sizeof(address), proto, config, b->class_plan,
                            e->connections, (uint8_t *)b->request, length,
                            response, b, e->created, e->released, e->socket_ctx,
                            &flow);
                else
                        rc = tg_flow_start_udp(
                            e->map, e->flows, (struct sockaddr *)&address,
                            sizeof(address), proto, config,
                            (uint8_t *)b->request, length, response, b,
                            e->created, e->released, e->socket_ctx, &flow);
                if (rc)
                        return -1;
        }
        if (s->kind == TG_STEP_HTTP)
                strcpy(flow->reuse_host, b->http.host);
        flow->deadline_cycles =
            b->step_start +
            (uint64_t)s->timeout_ms * e->scheduler->cycles_per_second / 1000;
        b->handle = flow->handle;
        b->waiting = true;
        return 0;
}

/** @copydoc tg_workflow_engine_init */
int tg_workflow_engine_init(struct tg_workflow_engine *e) {
        for (unsigned i = 0; i < e->plan->class_count; i++) {
                e->offsets[i] = e->step_count;
                if (e->plan->classes[i].proto == &tg_workflow_proto_ops)
                        e->step_count +=
                            ((struct tg_workflow_plan *)e->plan->classes[i]
                                 .proto_config)
                                ->count;
        }
        if (!e->step_count)
                return 0;
        /* Reserve per-shard business slots; response capture is allocated separately. */
        e->capacity = e->plan->max_concurrency;
        size_t n = (size_t)e->step_count * e->plan->phase_count;
        e->storage = calloc(e->capacity, sizeof(*e->storage));
        e->steps = calloc(n, sizeof(*e->steps));
        if (e->latency->groups)
                e->histograms = calloc(n * 3, sizeof(*e->histograms));
        if (!e->storage || !e->steps ||
            (e->latency->groups && !e->histograms)) {
                tg_workflow_engine_fini(e);
                return -1;
        }
        for (unsigned i = 0; i < e->capacity; i++) {
                e->storage[i].next = e->free;
                e->free = &e->storage[i];
        }
        for (size_t i = 0; i < n; i++)
                if (e->histograms)
                        e->steps[i].hist = &e->histograms[i * 3];
        e->memory_bytes = (uint64_t)e->capacity * sizeof(*e->storage) +
                          n * sizeof(*e->steps) +
                          (e->histograms ? n * 3 * sizeof(*e->histograms) : 0);
        return 0;
}

/** @copydoc tg_workflow_engine_fini */
void tg_workflow_engine_fini(struct tg_workflow_engine *e) {
        free(e->storage);
        free(e->steps);
        free(e->histograms);
        e->storage = NULL;
        e->steps = NULL;
        e->histograms = NULL;
}

/** @copydoc tg_workflow_start */
int tg_workflow_start(struct tg_workflow_engine *e,
                      const struct tg_class_plan *c) {
        struct tg_business *b = e->free;
        if (!b)
                return -1;
        e->free = b->next;
        memset(b, 0, sizeof(*b));
        b->engine = e;
        b->class_plan = c;
        b->plan = c->proto_config;
        b->cls = (unsigned)(c - e->plan->classes);
        b->phase = e->scheduler->phase_index;
        b->ordinal = e->scheduler->dispatch_ordinal;
        b->planned = e->scheduler->dispatch_planned_cycles;
        b->start = rte_get_timer_cycles();
        b->failure = TG_FLOW_RESULT_PROTOCOL_FAILURE;
        owner_timer_init(&b->timeout, timeout_cb, b);
        owner_timer_init(&b->wake, wake_cb, b);
        if (owner_timer_arm_after_ms(&b->timeout, b->plan->timeout_ms)) {
                b->next = e->free;
                e->free = b;
                return -1;
        }
        b->active = true;
        struct tg_flow f;
        timing(b, &f);
        if (e->latency->groups)
                tg_latency_on_admitted(e->latency, &f, b->phase, b->cls,
                                       b->planned);
        tg_stats_on_admitted(e->stats);
        enqueue(b);
        return 0;
}

/** @copydoc tg_workflow_tick */
void tg_workflow_tick(struct tg_workflow_engine *e, unsigned budget) {
        /* Charge every ready pop, including zero-delay branches, to avoid starvation. */
        while (budget-- && e->head) {
                struct tg_business *b = e->head;
                e->head = b->next;
                if (!e->head)
                        e->tail = NULL;
                b->queued = false;
                if (b->timed_out || rte_get_timer_cycles() - b->start >=
                                        (uint64_t)b->plan->timeout_ms *
                                            e->scheduler->cycles_per_second /
                                            1000) {
                        if ((b->waiting || b->thinking) &&
                            b->step < b->plan->count)
                                record_step(b, false, false);
                        b->failure = TG_FLOW_RESULT_IO_FAILURE;
                        finish(b, false);
                        continue;
                }
                if (b->failed) {
                        finish(b, false);
                        continue;
                }
                if (b->step == b->plan->count) {
                        finish(b, true);
                        continue;
                }
                const struct tg_step_plan *s = &b->plan->steps[b->step];
                if (b->thinking) {
                        b->thinking = false;
                        record_step(b, true, false);
                        advance(b, s->next);
                        enqueue(b);
                        continue;
                }
                b->step_start = rte_get_timer_cycles();
                b->visited |= 1U << b->step;
                step_stats(b, b->step)->counts.reached++;
                if (s->kind == TG_STEP_HTTP || s->kind == TG_STEP_DNS) {
                        if (network(b)) {
                                record_step(b, false, true);
                                b->failure =
                                    errno == ENOBUFS || errno == ENFILE
                                        ? TG_FLOW_RESULT_RESOURCE_PRESSURE
                                        : TG_FLOW_RESULT_PROTOCOL_FAILURE;
                                finish(b, false);
                        } else
                                step_stats(b, b->step)->counts.started++;
                } else {
                        step_stats(b, b->step)->counts.started++;
                        if (s->kind == TG_STEP_BRANCH) {
                                bool match;
                                if (predicate(b, s->condition, NULL, &match)) {
                                        record_step(b, false, false);
                                        finish(b, false);
                                } else {
                                        record_step(b, true, false);
                                        advance(b, match ? s->yes : s->no);
                                        enqueue(b);
                                }
                        } else {
                                /* Deterministic per planned arrival and step; owner
                                 * scheduling order never contributes to the seed.
                                 * Timer polling still limits actual wake precision. */
                                uint64_t x = b->ordinal +
                                             ((uint64_t)e->plan->seed << 32) +
                                             b->step +
                                             UINT64_C(0x9e3779b97f4a7c15);
                                x = (x ^ (x >> 30)) *
                                    UINT64_C(0xbf58476d1ce4e5b9);
                                x = (x ^ (x >> 27)) *
                                    UINT64_C(0x94d049bb133111eb);
                                x ^= x >> 31;
                                unsigned ms = s->min_ms +
                                              (unsigned)(x / 1U %
                                                         ((uint64_t)s->max_ms -
                                                          s->min_ms + 1));
                                b->thinking = true;
                                if (!ms)
                                        enqueue(b);
                                else if (owner_timer_arm_after_ms(&b->wake,
                                                                  ms)) {
                                        record_step(b, false, false);
                                        finish(b, false);
                                }
                        }
                }
        }
}

/** @copydoc tg_workflow_csv */
int tg_workflow_csv(FILE *f, const struct tg_plan *plan,
                    struct tg_workflow_engine *const *engines, unsigned count) {
        struct tg_histogram *merged = calloc(3, sizeof(*merged));
        if (!merged)
                return -1;
        const char *metrics[] = {"complete", "complete_success",
                                 "complete_failure"};
        for (unsigned p = 0; p < plan->phase_count; p++)
                for (unsigned c = 0; c < plan->class_count; c++) {
                        if (plan->classes[c].proto != &tg_workflow_proto_ops)
                                continue;
                        const struct tg_workflow_plan *wf =
                            plan->classes[c].proto_config;
                        for (unsigned step = 0; step < wf->count; step++) {
                                struct tg_step_counts counts = {0};
                                memset(merged, 0, 3 * sizeof(*merged));
                                for (unsigned w = 0; w < count; w++) {
                                        const struct tg_workflow_engine *e =
                                            engines[w];
                                        const struct tg_step_stats *s =
                                            &e->steps[p * e->step_count +
                                                      e->offsets[c] + step];
#define ADD(field) counts.field += s->counts.field
                                        ADD(reached);
                                        ADD(started);
                                        ADD(success);
                                        ADD(failed);
                                        ADD(start_failed);
                                        ADD(branch_skipped);
                                        ADD(not_reached);
#undef ADD
                                        if (s->hist)
                                                for (unsigned m = 0; m < 3; m++)
                                                        tg_histogram_merge(
                                                            &merged[m],
                                                            &s->hist[m]);
                                }
                                const char *protocol =
                                    wf->steps[step].kind == TG_STEP_HTTP
                                        ? "http"
                                    : wf->steps[step].kind == TG_STEP_DNS
                                        ? "dns"
                                    : wf->steps[step].kind == TG_STEP_THINK
                                        ? "think"
                                        : "branch";
                                for (unsigned m = 0; m < 3; m++)
                                        if (tg_latency_csv_step(
                                                f, plan->phases[p].name,
                                                plan->classes[c].name, protocol,
                                                wf->steps[step].name,
                                                metrics[m], &merged[m],
                                                &counts)) {
                                                free(merged);
                                                return -1;
                                        }
                        }
                }
        free(merged);
        return ferror(f) ? -1 : 0;
}
