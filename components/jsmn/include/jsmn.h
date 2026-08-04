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
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef JSMN_H
#define JSMN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * JSON type identifier.
 */
typedef enum {
    JSMN_PRIMITIVE = 0,
    JSMN_OBJECT = 1,
    JSMN_ARRAY = 2,
    JSMN_STRING = 3
} jsmntype_t;

typedef enum {
    JSMN_ERROR_NOMEM = -1,
    JSMN_ERROR_INVAL = -2,
    JSMN_ERROR_PART = -3
} jsmnerr_t;

/**
 * JSON token description.
 */
typedef struct jsmntok {
    jsmntype_t type;
    int start;
    int end;
    int size;
#ifdef JSMN_PARENT_LINKS
    int parent;
#endif
} jsmntok_t;

/**
 * JSON parser structure.
 */
typedef struct jsmn_parser {
    int pos;
    int toknext;
    int toksuper;
} jsmn_parser_t;

/* Compatibility alias: ADF uses jsmn_parser without _t suffix */
typedef jsmn_parser_t jsmn_parser;

/**
 * Initialize JSON parser.
 */
void jsmn_init(jsmn_parser_t *parser);

/**
 * Parse JSON string.
 */
int jsmn_parse(jsmn_parser_t *parser, const char *js, size_t len,
               jsmntok_t *tokens, unsigned int num_tokens);

#ifdef __cplusplus
}
#endif

#endif /* JSMN_H */