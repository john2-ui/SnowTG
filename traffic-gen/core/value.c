/**
 * @file value.c
 * @brief Strict JSON validation and bounded scalar extraction.
 *
 * jsmn provides token spans; a second pass enforces separators, scalar grammar,
 * and depth. Decoded values own their bytes so parser reset cannot invalidate
 * transaction context.
 */

#include "value.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Decodes four hexadecimal digits; the caller combines Unicode surrogates.
 */
static int hex4(const char *s, unsigned *v) {
        *v = 0;
        for (int i = 0; i < 4; i++) {
                unsigned char c = (unsigned char)s[i];
                if (!isxdigit(c))
                        return -1;
                *v = *v * 16 + (c <= '9' ? (unsigned)(c - '0')
                                         : (unsigned)tolower(c) - 'a' + 10);
        }
        return 0;
}

/** @copydoc tg_string_read */
int tg_string_read(const struct tg_json_doc *d, int t, char *out, size_t cap) {
        if (t < 0 || t >= d->token_count || d->tokens[t].type != JSMN_STRING ||
            !cap)
                return -1;
        size_t n = 0;
        for (int i = d->tokens[t].start; i < d->tokens[t].end; i++) {
                unsigned c = (unsigned char)d->text[i];
                if (c < 32)
                        return -1;
                if (c == '\\') {
                        if (++i >= d->tokens[t].end)
                                return -1;
                        c = (unsigned char)d->text[i];
                        const char *esc = "\"\\/bfnrt",
                                   *p = strchr(esc, (int)c);
                        if (p)
                                c = (unsigned char)"\"\\/\b\f\n\r\t"[p - esc];
                        else if (c == 'u') {
                                if (i + 4 >= d->tokens[t].end ||
                                    hex4(d->text + i + 1, &c))
                                        return -1;
                                i += 4;
                                if (c >= 0xd800 && c <= 0xdbff) {
                                        unsigned low;
                                        if (i + 6 >= d->tokens[t].end ||
                                            d->text[i + 1] != '\\' ||
                                            d->text[i + 2] != 'u' ||
                                            hex4(d->text + i + 3, &low) ||
                                            low < 0xdc00 || low > 0xdfff)
                                                return -1;
                                        c = 0x10000 + (c - 0xd800) * 1024 +
                                            low - 0xdc00;
                                        i += 6;
                                } else if (c >= 0xdc00 && c <= 0xdfff)
                                        return -1;
                                if (!c)
                                        return -1; /* NUL cannot be passed to C
                                                      protocol builders. */
                                unsigned bytes = c < 128     ? 1
                                                 : c < 2048  ? 2
                                                 : c < 65536 ? 3
                                                             : 4;
                                if (n + bytes >= cap)
                                        return -1;
                                if (bytes == 1)
                                        out[n++] = (char)c;
                                else {
                                        out[n++] =
                                            (char)((bytes == 2   ? 0xc0
                                                    : bytes == 3 ? 0xe0
                                                                 : 0xf0) |
                                                   (c >> (6 * (bytes - 1))));
                                        for (unsigned j = bytes - 1; j; j--)
                                                out[n++] =
                                                    (char)(0x80 |
                                                           ((c >>
                                                             (6 * (j - 1))) &
                                                            63));
                                }
                                continue;
                        } else
                                return -1;
                }
                if (c >= 128) {
                        unsigned bytes = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
                        unsigned cp = c & (0x7fU >> bytes);
                        if (c < 0xc2 || c > 0xf4 ||
                            i + (int)bytes > d->tokens[t].end ||
                            n + bytes >= cap)
                                return -1;
                        for (unsigned j = 1; j < bytes; j++) {
                                unsigned part =
                                    (unsigned char)d->text[i + (int)j];
                                if ((part & 0xc0) != 0x80)
                                        return -1;
                                cp = (cp << 6) | (part & 63);
                        }
                        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
                            (bytes == 3 && cp < 0x800) ||
                            (bytes == 4 && cp < 0x10000))
                                return -1;
                        memcpy(out + n, d->text + i, bytes);
                        n += bytes;
                        i += (int)bytes - 1;
                        continue;
                }
                if (n + 1 >= cap)
                        return -1;
                out[n++] = (char)c;
        }
        out[n] = 0;
        return 0;
}

/**
 * @brief Checks JSON numeric syntax before accepting a finite conversion.
 *
 * strtod alone would accept prefixes, leading-zero forms, or non-JSON spellings.
 */
static bool number(const char *s) {
        const unsigned char *p = (const unsigned char *)s;
        if (*p == '-')
                p++;
        if (*p == '0')
                p++;
        else {
                if (*p < '1' || *p > '9')
                        return false;
                while (isdigit(*p))
                        p++;
        }
        if (*p == '.') {
                p++;
                if (!isdigit(*p))
                        return false;
                while (isdigit(*p))
                        p++;
        }
        if (*p == 'e' || *p == 'E') {
                p++;
                if (*p == '+' || *p == '-')
                        p++;
                if (!isdigit(*p))
                        return false;
                while (isdigit(*p))
                        p++;
        }
        return !*p && isfinite(strtod(s, NULL));
}

/** @copydoc tg_value_read */
int tg_value_read(const struct tg_json_doc *d, int t, struct tg_value *v) {
        if (t < 0 || t >= d->token_count)
                return -1;
        if (d->tokens[t].type == JSMN_STRING) {
                v->kind = TG_VALUE_STRING;
                return tg_string_read(d, t, v->text, sizeof(v->text));
        }
        if (d->tokens[t].type != JSMN_PRIMITIVE)
                return -1;
        int len = d->tokens[t].end - d->tokens[t].start;
        if (len < 1 || len >= (int)sizeof(v->text))
                return -1;
        memcpy(v->text, d->text + d->tokens[t].start, (size_t)len);
        v->text[len] = 0;
        if (!strcmp(v->text, "null"))
                v->kind = TG_VALUE_NULL;
        else if (!strcmp(v->text, "true") || !strcmp(v->text, "false"))
                v->kind = TG_VALUE_BOOL;
        else if (number(v->text))
                v->kind = TG_VALUE_NUMBER;
        else
                return -1;
        return 0;
}

/** @copydoc tg_json_next */
int tg_json_next(const struct tg_json_doc *d, int t) {
        int end = d->tokens[t].end;
        t++;
        while (t < d->token_count && d->tokens[t].start < end)
                t++;
        return t;
}
static void ws(const char *s, int *p) {
        while (s[*p] && strchr(" \t\r\n", s[*p]))
                (*p)++;
}
/* jsmn supplies spans; this pass enforces separators, scalar grammar and depth
 * (jsmn alone accepts some malformed JSON). No response-sized C stack buffer.
 */
static int valid(const struct tg_json_doc *d, int *t, int *p, unsigned depth) {
        if (depth > 64 || *t >= d->token_count)
                return -1;
        const jsmntok_t *tok = &d->tokens[*t];
        ws(d->text, p);
        if (tok->type == JSMN_STRING) {
                if (*p != tok->start - 1)
                        return -1;
                size_t cap = (size_t)(tok->end - tok->start) + 1;
                char *s = malloc(cap);
                if (!s)
                        return -1;
                int rc = tg_string_read(d, *t, s, cap);
                free(s);
                if (rc)
                        return -1;
                *p = tok->end + 1;
                (*t)++;
                return 0;
        }
        if (*p != tok->start)
                return -1;
        if (tok->type == JSMN_PRIMITIVE) {
                struct tg_value v;
                if (tg_value_read(d, *t, &v))
                        return -1;
                *p = tok->end;
                (*t)++;
                return 0;
        }
        bool object = tok->type == JSMN_OBJECT;
        char end = object ? '}' : ']';
        (*p)++;
        (*t)++;
        ws(d->text, p);
        if (d->text[*p] == end) {
                (*p)++;
                return 0;
        }
        for (;;) {
                if (object) {
                        if (*t >= d->token_count ||
                            d->tokens[*t].type != JSMN_STRING ||
                            valid(d, t, p, depth + 1))
                                return -1;
                        ws(d->text, p);
                        if (d->text[(*p)++] != ':')
                                return -1;
                }
                if (valid(d, t, p, depth + 1))
                        return -1;
                ws(d->text, p);
                if (d->text[*p] == end) {
                        (*p)++;
                        return 0;
                }
                if (d->text[(*p)++] != ',')
                        return -1;
        }
}

/** @copydoc tg_document_parse */
int tg_document_parse(struct tg_document *out, const char *s, size_t n) {
        memset(out, 0, sizeof(*out));
        if (n > 32U * 1024U * 1024U || memchr(s, 0, n))
                return -1;
        out->text = malloc(n + 1);
        if (!out->text)
                return -1;
        memcpy(out->text, s, n);
        out->text[n] = 0;
        /* Grow only at startup or for an explicitly captured response body. */
        /* Text and token storage are reclaimed together on every failure path. */
        unsigned cap = 128;
        int count;
        for (;;) {
                out->tokens = calloc(cap, sizeof(*out->tokens));
                if (!out->tokens)
                        goto fail;
                jsmn_parser p;
                jsmn_init(&p);
                count = jsmn_parse(&p, out->text, n, out->tokens, cap);
                if (count != JSMN_ERROR_NOMEM)
                        break;
                free(out->tokens);
                out->tokens = NULL;
                if (cap > 4U * 1024U * 1024U)
                        goto fail;
                cap *= 2;
        }
        out->view = (struct tg_json_doc){out->text, out->tokens, count};
        int t = 0, p = 0;
        if (count < 1 || valid(&out->view, &t, &p, 0))
                goto fail;
        ws(out->text, &p);
        if ((size_t)p != n || t != count)
                goto fail;
        return 0;
fail:
        tg_document_free(out);
        errno = EINVAL;
        return -1;
}

/** @copydoc tg_document_free */
void tg_document_free(struct tg_document *d) {
        free(d->text);
        free(d->tokens);
        memset(d, 0, sizeof(*d));
}

/** @copydoc tg_json_pointer */
int tg_json_pointer(const struct tg_json_doc *d, const char *path) {
        int t = 0;
        while (*path) {
                if (*path++ != '/')
                        return -1;
                char key[TG_VALUE_CAP];
                size_t n = 0;
                while (*path && *path != '/') {
                        char c = *path++;
                        if (c == '~') {
                                if (*path != '0' && *path != '1')
                                        return -1;
                                c = *path++ == '0' ? '~' : '/';
                        }
                        if (n + 1 >= sizeof(key))
                                return -1;
                        key[n++] = c;
                }
                key[n] = 0;
                int found = -1;
                if (d->tokens[t].type == JSMN_OBJECT) {
                        for (int i = t + 1;
                             i < d->token_count &&
                             d->tokens[i].start < d->tokens[t].end;) {
                                char name[TG_VALUE_CAP];
                                if (tg_string_read(d, i, name, sizeof(name)))
                                        return -1;
                                if (!strcmp(name, key)) {
                                        if (found >= 0)
                                                return -1;
                                        found = i + 1;
                                }
                                i = tg_json_next(d, i + 1);
                        }
                } else if (d->tokens[t].type == JSMN_ARRAY) {
                        if (!n || (n > 1 && key[0] == '0'))
                                return -1;
                        unsigned long index = 0;
                        for (size_t j = 0; j < n; j++) {
                                if (!isdigit((unsigned char)key[j]) ||
                                    index > 1000000)
                                        return -1;
                                index = index * 10 + key[j] - '0';
                        }
                        int i = t + 1;
                        while (index-- && i < d->token_count)
                                i = tg_json_next(d, i);
                        if (i < d->token_count && d->tokens[i].parent == t)
                                found = i;
                }
                if (found < 0)
                        return -1;
                t = found;
        }
        return t;
}

/** @copydoc tg_value_compare */
int tg_value_compare(const struct tg_value *a, const char *op,
                     const struct tg_value *b, bool *out) {
        if (!a || !b || a->kind != b->kind)
                return -1;
        int cmp;
        if (a->kind == TG_VALUE_NUMBER) {
                long double x = strtold(a->text, NULL),
                            y = strtold(b->text, NULL);
                cmp = (x > y) - (x < y);
        } else
                cmp = strcmp(a->text, b->text);
        if (!strcmp(op, "=="))
                *out = cmp == 0;
        else if (!strcmp(op, "!="))
                *out = cmp != 0;
        else if (a->kind != TG_VALUE_NUMBER && a->kind != TG_VALUE_STRING)
                return -1;
        else if (!strcmp(op, "<"))
                *out = cmp < 0;
        else if (!strcmp(op, "<="))
                *out = cmp <= 0;
        else if (!strcmp(op, ">"))
                *out = cmp > 0;
        else if (!strcmp(op, ">="))
                *out = cmp >= 0;
        else
                return -1;
        return 0;
}
