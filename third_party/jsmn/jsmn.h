/*
 * MIT License
 *
 * Copyright (c) 2010 Serge Zaitsev
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 */
#ifndef JSMN_H
#define JSMN_H

#include <stddef.h>

typedef enum {
        JSMN_UNDEFINED = 0,
        JSMN_OBJECT = 1 << 0,
        JSMN_ARRAY = 1 << 1,
        JSMN_STRING = 1 << 2,
        JSMN_PRIMITIVE = 1 << 3
} jsmntype_t;

enum jsmnerr {
        JSMN_ERROR_NOMEM = -1,
        JSMN_ERROR_INVAL = -2,
        JSMN_ERROR_PART = -3
};

typedef struct {
        jsmntype_t type;
        int start;
        int end;
        int size;
        int parent;
} jsmntok_t;

typedef struct {
        unsigned int pos;
        unsigned int toknext;
        int toksuper;
} jsmn_parser;

static void jsmn_init(jsmn_parser *parser) {
        parser->pos = 0;
        parser->toknext = 0;
        parser->toksuper = -1;
}

static jsmntok_t *jsmn_alloc_token(jsmn_parser *parser, jsmntok_t *tokens,
                                    size_t token_count) {
        jsmntok_t *token;

        if (parser->toknext >= token_count)
                return NULL;
        token = &tokens[parser->toknext++];
        token->start = -1;
        token->end = -1;
        token->size = 0;
        token->parent = -1;
        return token;
}

static void jsmn_fill_token(jsmntok_t *token, jsmntype_t type, int start,
                            int end) {
        token->type = type;
        token->start = start;
        token->end = end;
        token->size = 0;
}

static int jsmn_parse_primitive(jsmn_parser *parser, const char *json,
                                size_t length, jsmntok_t *tokens,
                                size_t token_count) {
        int start = (int)parser->pos;
        jsmntok_t *token;

        for (; parser->pos < length; parser->pos++) {
                switch (json[parser->pos]) {
                case '\t':
                case '\r':
                case '\n':
                case ' ':
                case ',':
                case ']':
                case '}':
                        goto found;
                default:
                        if ((unsigned char)json[parser->pos] < 32 ||
                            (unsigned char)json[parser->pos] >= 127)
                                return JSMN_ERROR_INVAL;
                }
        }
found:
        token = jsmn_alloc_token(parser, tokens, token_count);
        if (token == NULL) {
                parser->pos = (unsigned int)start;
                return JSMN_ERROR_NOMEM;
        }
        jsmn_fill_token(token, JSMN_PRIMITIVE, start, (int)parser->pos);
        token->parent = parser->toksuper;
        parser->pos--;
        return 0;
}

static int jsmn_parse_string(jsmn_parser *parser, const char *json,
                             size_t length, jsmntok_t *tokens,
                             size_t token_count) {
        int start = (int)parser->pos;
        jsmntok_t *token;

        parser->pos++;
        for (; parser->pos < length; parser->pos++) {
                char c = json[parser->pos];

                if (c == '"') {
                        token = jsmn_alloc_token(parser, tokens, token_count);
                        if (token == NULL) {
                                parser->pos = (unsigned int)start;
                                return JSMN_ERROR_NOMEM;
                        }
                        jsmn_fill_token(token, JSMN_STRING, start + 1,
                                        (int)parser->pos);
                        token->parent = parser->toksuper;
                        return 0;
                }
                if (c == '\\') {
                        parser->pos++;
                        if (parser->pos >= length)
                                break;
                        switch (json[parser->pos]) {
                        case '"':
                        case '/':
                        case '\\':
                        case 'b':
                        case 'f':
                        case 'r':
                        case 'n':
                        case 't':
                                break;
                        case 'u':
                                for (int i = 0; i < 4; i++) {
                                        parser->pos++;
                                        if (parser->pos >= length ||
                                            !((json[parser->pos] >= '0' &&
                                               json[parser->pos] <= '9') ||
                                              (json[parser->pos] >= 'A' &&
                                               json[parser->pos] <= 'F') ||
                                              (json[parser->pos] >= 'a' &&
                                               json[parser->pos] <= 'f')))
                                                return JSMN_ERROR_INVAL;
                                }
                                break;
                        default:
                                return JSMN_ERROR_INVAL;
                        }
                }
        }
        parser->pos = (unsigned int)start;
        return JSMN_ERROR_PART;
}

static int jsmn_parse(jsmn_parser *parser, const char *json, size_t length,
                      jsmntok_t *tokens, size_t token_count) {
        int count = 0;

        for (; parser->pos < length; parser->pos++) {
                jsmntok_t *token;
                int result;

                switch (json[parser->pos]) {
                case '{':
                case '[':
                        token = jsmn_alloc_token(parser, tokens, token_count);
                        if (token == NULL)
                                return JSMN_ERROR_NOMEM;
                        if (parser->toksuper != -1)
                                tokens[parser->toksuper].size++;
                        token->type =
                            json[parser->pos] == '{' ? JSMN_OBJECT : JSMN_ARRAY;
                        token->start = (int)parser->pos;
                        token->parent = parser->toksuper;
                        parser->toksuper = (int)parser->toknext - 1;
                        count++;
                        break;
                case '}':
                case ']':
                        for (int i = (int)parser->toknext - 1; i >= 0; i--) {
                                token = &tokens[i];
                                if (token->start != -1 && token->end == -1) {
                                        jsmntype_t type = json[parser->pos] == '}'
                                                              ? JSMN_OBJECT
                                                              : JSMN_ARRAY;

                                        if (token->type != type)
                                                return JSMN_ERROR_INVAL;
                                        token->end = (int)parser->pos + 1;
                                        parser->toksuper = token->parent;
                                        break;
                                }
                                if (i == 0)
                                        return JSMN_ERROR_INVAL;
                        }
                        break;
                case '"':
                        result = jsmn_parse_string(parser, json, length, tokens,
                                                   token_count);
                        if (result < 0)
                                return result;
                        count++;
                        if (parser->toksuper != -1)
                                tokens[parser->toksuper].size++;
                        break;
                case '\t':
                case '\r':
                case '\n':
                case ' ':
                        break;
                case ':':
                        parser->toksuper = (int)parser->toknext - 1;
                        break;
                case ',':
                        if (parser->toksuper != -1 &&
                            tokens[parser->toksuper].type != JSMN_ARRAY &&
                            tokens[parser->toksuper].type != JSMN_OBJECT) {
                                parser->toksuper =
                                    tokens[parser->toksuper].parent;
                        }
                        break;
                default:
                        result = jsmn_parse_primitive(parser, json, length,
                                                      tokens, token_count);
                        if (result < 0)
                                return result;
                        count++;
                        if (parser->toksuper != -1)
                                tokens[parser->toksuper].size++;
                        break;
                }
        }
        for (unsigned int i = 0; i < parser->toknext; i++)
                if (tokens[i].start != -1 && tokens[i].end == -1)
                        return JSMN_ERROR_PART;
        return count;
}

#endif /* JSMN_H */
