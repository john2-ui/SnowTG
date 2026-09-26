/**
 * @file workflow_plan.c
 * @brief Compiles immutable transaction configuration before traffic starts.
 *
 * The first pass registers step names; the second resolves forward targets and
 * validates templates, selectors, bounds, and context capacity. Runtime-only
 * values are checked again when their step prepares a request.
 */
#include "workflow_plan.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @copydoc tg_wf_member */
int tg_wf_member(const struct tg_workflow_plan *p, int o, const char *key) {
        const struct tg_json_doc *d = &p->doc.view;
        if (o < 0 || o >= d->token_count || d->tokens[o].type != JSMN_OBJECT)
                return -1;
        int found = -1;
        for (int i = o + 1;
             i < d->token_count && d->tokens[i].start < d->tokens[o].end;
             i = tg_json_next(d, i + 1)) {
                char name[TG_VALUE_CAP];
                /* Configuration and data keys use the same decoded spelling
                 * as template references, including JSON Unicode escapes. */
                if (tg_string_read(d, i, name, sizeof(name)))
                        return -1;
                if (!strcmp(name, key)) {
                        if (found >= 0)
                                return -1;
                        found = i + 1;
                }
        }
        return found;
}

/** @copydoc tg_wf_text */
int tg_wf_text(const struct tg_workflow_plan *p, int o, const char *key,
               const char *fallback, char *out, size_t cap) {
        int t = tg_wf_member(p, o, key);
        if (t >= 0)
                return tg_string_read(&p->doc.view, t, out, cap);
        if (!fallback || strlen(fallback) >= cap)
                return -1;
        strcpy(out, fallback);
        return 0;
}

/** @copydoc tg_wf_u32 */
int tg_wf_u32(const struct tg_workflow_plan *p, int o, const char *key,
              unsigned fallback, unsigned min, unsigned max, unsigned *out) {
        int t = tg_wf_member(p, o, key);
        *out = fallback;
        return t < 0 ? 0
                     : tg_json_parse_u32(&p->doc.view, &p->doc.tokens[t], min,
                                         max, out);
}

/**
 * @brief Rejects unknown and duplicate members against a field whitelist.
 *
 * Space-delimited names avoid partial matches and ambiguous last-key-wins rules.
 */
static int object(const struct tg_workflow_plan *p, int o,
                  const char *allowed) {
        const struct tg_json_doc *d = &p->doc.view;
        if (o < 0 || d->tokens[o].type != JSMN_OBJECT)
                return -1;
        for (int i = o + 1;
             i < d->token_count && d->tokens[i].start < d->tokens[o].end;) {
                char name[64], match[68];
                if (tg_string_read(d, i, name, sizeof(name)))
                        return -1;
                snprintf(match, sizeof(match), " %s ", name);
                if (!strstr(allowed, match))
                        return -1;
                for (int j = o + 1; j < i; j = tg_json_next(d, j + 1)) {
                        char other[64];
                        if (tg_string_read(d, j, other, sizeof(other)) ||
                            !strcmp(name, other))
                                return -1;
                }
                i = tg_json_next(d, i + 1);
        }
        return 0;
}

/**
 * @brief Validates a vars object or dataset row as at most 32 unique scalar fields.
 *
 * Absent optional vars are allowed; nested objects and arrays are not values.
 */
static int values(const struct tg_workflow_plan *p, int o) {
        if (o < 0)
                return 0;
        const struct tg_json_doc *d = &p->doc.view;
        if (d->tokens[o].type != JSMN_OBJECT ||
            d->tokens[o].size > (int)TG_WF_VARS)
                return -1;
        unsigned count = 0;
        for (int i = o + 1;
             i < d->token_count && d->tokens[i].start < d->tokens[o].end;
             i = tg_json_next(d, i + 1)) {
                char name[64];
                struct tg_value v;
                if (++count > TG_WF_VARS ||
                    tg_string_read(d, i, name, sizeof(name)) || !name[0] ||
                    tg_value_read(d, i + 1, &v))
                        return -1;
                for (int j = o + 1; j < i; j = tg_json_next(d, j + 1)) {
                        char other[64];
                        if (tg_string_read(d, j, other, sizeof(other)) ||
                            !strcmp(other, name))
                                return -1;
                }
        }
        return 0;
}

/**
 * @brief Validates a context reference or protocol-specific response selector.
 *
 * Branches only allow references. HTTP permits status/Header/JSON scalar exports;
 * DNS permits a matching IPv4 address. ref and source are mutually exclusive.
 */
static int selector(const struct tg_workflow_plan *p, int o,
                    enum tg_step_kind kind, bool response) {
        if (object(p, o, " ref  source  path  index "))
                return -1;
        char ref[128], source[32], path[TG_VALUE_CAP];
        unsigned index;
        int rt = tg_wf_member(p, o, "ref"), st = tg_wf_member(p, o, "source");
        if ((rt >= 0) == (st >= 0))
                return -1;
        if (rt >= 0)
                return tg_wf_text(p, o, "ref", NULL, ref, sizeof(ref)) ||
                               !(strchr(ref, '.') &&
                                 (!strncmp(ref, "vars.", 5) ||
                                  !strncmp(ref, "data.", 5) ||
                                  !strncmp(ref, "ctx.", 4)))
                           ? -1
                           : 0;
        if (!response ||
            tg_wf_text(p, o, "source", NULL, source, sizeof(source)) ||
            tg_wf_text(p, o, "path", "", path, sizeof(path)) ||
            tg_wf_u32(p, o, "index", 0, 0, 65535, &index))
                return -1;
        if (kind == TG_STEP_DNS)
                return strcmp(source, "address") ? -1 : 0;
        return !strcmp(source, "status") ||
                       (!strcmp(source, "header") && path[0]) ||
                       (!strcmp(source, "json") && (!path[0] || path[0] == '/'))
                   ? 0
                   : -1;
}

/**
 * @brief Checks the supported comparison operator and typed literal right operand.
 *
 * exists has no right operand. Runtime comparison validates the resolved left type.
 */
static int condition(const struct tg_workflow_plan *p, int o,
                     enum tg_step_kind kind, bool response) {
        char op[16];
        struct tg_value v;
        if (object(p, o, " left  op  right ") ||
            tg_wf_text(p, o, "op", NULL, op, sizeof(op)) ||
            selector(p, tg_wf_member(p, o, "left"), kind, response))
                return -1;
        if (!strcmp(op, "exists"))
                return tg_wf_member(p, o, "right") >= 0 ? -1 : 0;
        if (strcmp(op, "==") && strcmp(op, "!=") && strcmp(op, "<") &&
            strcmp(op, "<=") && strcmp(op, ">") && strcmp(op, ">="))
                return -1;
        return tg_value_read(&p->doc.view, tg_wf_member(p, o, "right"), &v);
}

/** @brief Compiles capture requirements from validated selectors only. */
static void capture_source(const struct tg_workflow_plan *p, int selector,
                           struct tg_step_plan *step) {
        char source[16];
        if (tg_wf_text(p, selector, "source", "", source, sizeof(source)))
                return;
        step->capture_headers |= !strcmp(source, "header");
        step->capture_json |= !strcmp(source, "json");
}

/**
 * @brief Resolves a forward step name or end sentinel.
 *
 * Rejecting backward/self jumps bounds execution without runtime loop detection.
 */
static int target(const struct tg_workflow_plan *p, int o, const char *key,
                  unsigned from, unsigned *out) {
        char name[64];
        if (tg_wf_text(p, o, key, NULL, name, sizeof(name)))
                return -1;
        if (!strcmp(name, "end")) {
                *out = p->count;
                return 0;
        }
        for (unsigned i = from + 1; i < p->count; i++)
                if (!strcmp(name, p->steps[i].name)) {
                        *out = i;
                        return 0;
                }
        return -1;
}

/**
 * @brief Validates template syntax without resolving runtime context.
 *
 * Response-derived ctx may not exist yet. Expanded lengths and protocol field
 * validity are checked again at step execution.
 */
static int template_field(const struct tg_workflow_plan *p, int o,
                          const char *key, const char *fallback, size_t cap) {
        char *text = malloc(cap);
        if (!text)
                return -1;
        int rc = tg_wf_text(p, o, key, fallback, text, cap);
        if (!rc)
                for (const char *s = text; (s = strstr(s, "${")) != NULL;) {
                        const char *end = strchr(s + 2, '}');
                        if (!end || end - s > 128) {
                                rc = -1;
                                break;
                        }
                        char ref[128];
                        size_t n = (size_t)(end - s - 2);
                        memcpy(ref, s + 2, n);
                        ref[n] = 0;
                        char *filter = strchr(ref, '|');
                        if (filter)
                                *filter++ = 0;
                        const char *dot = strchr(ref, '.');
                        if (!dot || !dot[1] ||
                            (strncmp(ref, "vars.", 5) &&
                             strncmp(ref, "data.", 5) &&
                             strncmp(ref, "ctx.", 4)) ||
                            (filter && strcmp(filter, "url") &&
                             strcmp(filter, "json"))) {
                                rc = -1;
                                break;
                        }
                        s = end + 1;
                }
        free(text);
        return rc;
}

/**
 * @brief Compiles both passes against an already-owned JSON document.
 *
 * Dataset rows retain token indices rather than copying every field. Context
 * capacity bounds the union of extraction names across all steps. The default
 * business timeout sums all network deadlines and maximum think durations.
 */
static int compile_owned(struct tg_workflow_plan *p) {
        const struct tg_json_doc *d = &p->doc.view;
        if (object(p, 0, " vars  dataset  steps  timeout_ms "))
                return -1;
        p->vars = tg_wf_member(p, 0, "vars");
        if (values(p, p->vars))
                return -1;
        int dataset = tg_wf_member(p, 0, "dataset");
        if (dataset >= 0) {
                if (d->tokens[dataset].type != JSMN_ARRAY) {
                        fprintf(stderr, "workflow dataset: use snowtg.py to "
                                        "expand file references\n");
                        return -1;
                }
                if ((unsigned)(d->tokens[dataset].end -
                               d->tokens[dataset].start) > 16U * 1024U * 1024U)
                        return -1;
                p->row_count = (unsigned)d->tokens[dataset].size;
                if (!p->row_count)
                        return -1;
                p->rows = calloc(p->row_count, sizeof(*p->rows));
                if (!p->rows)
                        return -1;
                unsigned row = 0;
                for (int i = dataset + 1;
                     i < d->token_count &&
                     d->tokens[i].start < d->tokens[dataset].end;
                     i = tg_json_next(d, i)) {
                        if (row >= p->row_count || values(p, i))
                                return -1;
                        p->rows[row++] = i;
                }
        }
        int steps = tg_wf_member(p, 0, "steps");
        if (steps < 0 || d->tokens[steps].type != JSMN_ARRAY ||
            d->tokens[steps].size < 1 ||
            d->tokens[steps].size > (int)TG_WF_STEPS)
                return -1;
        for (int i = steps + 1;
             i < d->token_count && d->tokens[i].start < d->tokens[steps].end;
             i = tg_json_next(d, i)) {
                struct tg_step_plan *s = &p->steps[p->count];
                char kind[16];
                if (tg_wf_text(p, i, "name", NULL, s->name, sizeof(s->name)) ||
                    !s->name[0] || !strcmp(s->name, "end") ||
                    tg_wf_text(p, i, "type", NULL, kind, sizeof(kind)))
                        return -1;
                for (unsigned j = 0; j < p->count; j++)
                        if (!strcmp(s->name, p->steps[j].name))
                                return -1;
                if (!strcmp(kind, "http"))
                        s->kind = TG_STEP_HTTP;
                else if (!strcmp(kind, "dns"))
                        s->kind = TG_STEP_DNS;
                else if (!strcmp(kind, "think"))
                        s->kind = TG_STEP_THINK;
                else if (!strcmp(kind, "branch"))
                        s->kind = TG_STEP_BRANCH;
                else
                        return -1;
                s->object = i;
                p->count++;
        }
        /* All names are now known; resolve forward targets in the second pass. */
        uint64_t timeout = 0;
        char context_names[TG_WF_VARS][64];
        unsigned context_count = 0;
        for (unsigned i = 0; i < p->count; i++) {
                struct tg_step_plan *s = &p->steps[i];
                int o = s->object;
                const char *fields =
                    s->kind == TG_STEP_HTTP  ? " name  type  next  peer  http  "
                                               "extract  checks  timeout_ms "
                    : s->kind == TG_STEP_DNS ? " name  type  next  peer  dns  "
                                               "extract  checks  timeout_ms "
                    : s->kind == TG_STEP_THINK
                        ? " name  type  next  ms  min_ms  max_ms "
                        : " name  type  condition  then  else ";
                if (object(p, o, fields))
                        return -1;
                s->next = i + 1;
                if (tg_wf_member(p, o, "next") >= 0 &&
                    target(p, o, "next", i, &s->next))
                        return -1;
                s->extract = tg_wf_member(p, o, "extract");
                s->checks = tg_wf_member(p, o, "checks");
                if (s->kind == TG_STEP_BRANCH) {
                        s->condition = tg_wf_member(p, o, "condition");
                        if (condition(p, s->condition, s->kind, false) ||
                            target(p, o, "then", i, &s->yes) ||
                            target(p, o, "else", i, &s->no))
                                return -1;
                } else if (s->kind == TG_STEP_THINK) {
                        unsigned ms;
                        if (tg_wf_u32(p, o, "ms", 0, 0, 3600000, &ms) ||
                            tg_wf_u32(p, o, "min_ms", ms, 0, 3600000,
                                      &s->min_ms) ||
                            tg_wf_u32(p, o, "max_ms", ms, 0, 3600000,
                                      &s->max_ms) ||
                            s->min_ms > s->max_ms ||
                            (tg_wf_member(p, o, "ms") >= 0 &&
                             (tg_wf_member(p, o, "min_ms") >= 0 ||
                              tg_wf_member(p, o, "max_ms") >= 0)))
                                return -1;
                        timeout += s->max_ms;
                } else {
                        unsigned port;
                        char ip[TG_VALUE_CAP];
                        int peer = tg_wf_member(p, o, "peer");
                        if (object(p, peer, " ip  port ") ||
                            tg_wf_text(p, peer, "ip", NULL, ip, sizeof(ip)) ||
                            !ip[0] ||
                            tg_wf_u32(p, peer, "port",
                                      s->kind == TG_STEP_HTTP ? 80 : 53, 1,
                                      65535, &port) ||
                            tg_wf_u32(p, o, "timeout_ms", 5000, 1, 3600000,
                                      &s->timeout_ms))
                                return -1;
                        timeout += s->timeout_ms;
                        int cfg = tg_wf_member(
                            p, o, s->kind == TG_STEP_HTTP ? "http" : "dns");
                        if (object(p, cfg,
                                   s->kind == TG_STEP_HTTP
                                       ? " method  path  host  keepalive  "
                                         "headers  body "
                                       : " qname  qtype "))
                                return -1;
                        if (template_field(p, peer, "ip", NULL, TG_VALUE_CAP))
                                return -1;
                        if (s->kind == TG_STEP_HTTP) {
                                if (template_field(p, cfg, "method", "GET",
                                                   16) ||
                                    template_field(p, cfg, "path", "/",
                                                   TG_WF_REQUEST_CAP) ||
                                    template_field(p, cfg, "host", "",
                                                   TG_VALUE_CAP) ||
                                    template_field(p, cfg, "body", "",
                                                   TG_WF_REQUEST_CAP))
                                        return -1;
                                int keep = tg_wf_member(p, cfg, "keepalive"),
                                    headers = tg_wf_member(p, cfg, "headers");
                                bool enabled;
                                if (keep >= 0 &&
                                    tg_json_parse_bool(d, &d->tokens[keep],
                                                       &enabled))
                                        return -1;
                                if (headers >= 0) {
                                        if (d->tokens[headers].type !=
                                                JSMN_OBJECT ||
                                            d->tokens[headers].size > 32)
                                                return -1;
                                        for (int j = headers + 1;
                                             j < d->token_count &&
                                             d->tokens[j].start <
                                                 d->tokens[headers].end;
                                             j = tg_json_next(d, j + 1)) {
                                                char key[128];
                                                if (tg_string_read(
                                                        d, j, key,
                                                        sizeof(key)) ||
                                                    !key[0] ||
                                                    template_field(
                                                        p, headers, key, NULL,
                                                        TG_WF_REQUEST_CAP))
                                                        return -1;
                                        }
                                }
                        } else if (template_field(p, cfg, "qname", NULL,
                                                  TG_VALUE_CAP))
                                return -1;
                        if (s->kind == TG_STEP_DNS) {
                                char q[16];
                                if (tg_wf_text(p, cfg, "qtype", "A", q,
                                               sizeof(q)) ||
                                    strcmp(q, "A"))
                                        return -1;
                        }
                        if (s->extract >= 0) {
                                if (d->tokens[s->extract].type != JSMN_OBJECT ||
                                    d->tokens[s->extract].size >
                                        (int)TG_WF_VARS)
                                        return -1;
                                for (int j = s->extract + 1;
                                     j < d->token_count &&
                                     d->tokens[j].start <
                                         d->tokens[s->extract].end;
                                     j = tg_json_next(d, j + 1)) {
                                        char name[64];
                                        if (tg_string_read(d, j, name,
                                                           sizeof(name)) ||
                                            !name[0] ||
                                            selector(p, j + 1, s->kind, true))
                                                return -1;
                                        capture_source(p, j + 1, s);
                                        unsigned k = 0;
                                        for (; k < context_count; k++)
                                                if (!strcmp(name,
                                                            context_names[k]))
                                                        break;
                                        if (k == context_count) {
                                                if (context_count == TG_WF_VARS)
                                                        return -1;
                                                strcpy(context_names
                                                           [context_count++],
                                                       name);
                                        }
                                }
                        }
                        if (s->checks >= 0) {
                                if (d->tokens[s->checks].type != JSMN_ARRAY ||
                                    d->tokens[s->checks].size > 32)
                                        return -1;
                                for (int j = s->checks + 1;
                                     j < d->token_count &&
                                     d->tokens[j].start <
                                         d->tokens[s->checks].end;
                                     j = tg_json_next(d, j)) {
                                        if (condition(p, j, s->kind, true))
                                                return -1;
                                        capture_source(p, tg_wf_member(p, j, "left"), s);
                                }
                        }
                }
        }
        if (!timeout)
                timeout = 1;
        if (timeout > 86400000 ||
            tg_wf_u32(p, 0, "timeout_ms", (unsigned)timeout, 1, 86400000,
                      &p->timeout_ms))
                return -1;
        return 0;
}

/**
 * @brief Releases a complete or partially compiled plan and its row index array.
 */
static void destroy(void *ptr) {
        struct tg_workflow_plan *p = ptr;
        if (p) {
                tg_document_free(&p->doc);
                free(p->rows);
                free(p);
        }
}

/**
 * @brief Copies, parses, and compiles a subtree before publishing it to the caller.
 *
 * The same path supports shard cloning; all partial allocations are reclaimed
 * on failure, and no token can outlive its owned source text.
 */
static int copy_text(const char *text, size_t n, void **out) {
        struct tg_workflow_plan *p = calloc(1, sizeof(*p));
        *out = NULL;
        if (!p)
                return -1;
        if (tg_document_parse(&p->doc, text, n) || compile_owned(p)) {
                destroy(p);
                errno = EINVAL;
                return -1;
        }
        *out = p;
        return 0;
}

/** @copydoc tg_workflow_compile */
int tg_workflow_compile(const struct tg_json_doc *d, int t, void **out) {
        return copy_text(d->text + d->tokens[t].start,
                         (size_t)(d->tokens[t].end - d->tokens[t].start), out);
}

/**
 * @brief Rebuilds an independent immutable plan for another shard.
 */
static int clone(const void *src, void **dst) {
        const struct tg_workflow_plan *p = src;
        return copy_text(p->doc.text, strlen(p->doc.text), dst);
}

/**
 * @brief Class lifecycle/statistics tag, never a wire-protocol implementation.
 *
 * The scheduler routes this class to the business engine. Actual Flows always
 * use HTTP or DNS callbacks; this vtable only clones and releases configuration.
 */
const struct tg_proto_ops tg_workflow_proto_ops = {
    .name = "transaction", .config_clone = clone, .config_free = destroy};
