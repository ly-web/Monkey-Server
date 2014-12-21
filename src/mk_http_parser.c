/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Monkey HTTP Server
 *  ==================
 *  Copyright 2001-2014 Monkey Software LLC <eduardo@monkey.io>
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include <ctype.h>

#include <monkey/mk_http.h>
#include <monkey/mk_http_parser.h>
#include <monkey/mk_http_status.h>

#define mark_end()                              \
    p->end = p->i;                              \
    p->chars = -1;

#define parse_next()                            \
    p->start = p->i + 1;                        \
    continue

#define field_len()   (p->end - p->start)
#define header_scope_eq(p, x) p->header_min = p->header_max = x

struct row_entry {
    int len;
    const char name[32];
};

struct row_entry mk_methods_table[] = {
    { 3, "GET"     },
    { 4, "POST"    },
    { 4, "HEAD"    },
    { 3, "PUT"     },
    { 6, "DELETE"  },
    { 7, "OPTIONS" }
};

struct row_entry mk_headers_table[] = {
    {  6, "accept"              },
    { 14, "accept-charset"      },
    { 15, "accept-encoding"     },
    { 15, "accept-language"     },
    { 13, "authorization"       },
    {  6, "cookie"              },
    { 10, "connection"          },
    { 14, "content-length"      },
    { 13, "content-range"       },
    { 12, "content-type"        },
    { 17, "if-modified-since"   },
    {  4, "host"                },
    { 13, "last-modified"       },
    { 19, "last-modified-since" },
    {  7, "referer"             },
    {  5, "range"               },
    { 10, "user-agent"          }
};

static inline int str_searchr(char *buf, char c, int len)
{
    int i;

    for (i = len - 1; i >= 0; i--) {
        if (buf[i] == c) {
            return i;
        }
    }

    return -1;
}

/*
 * expected: a known & expected value in lowercase
 * value   : mk_ptr_t points the header value
 *
 * If it matches it return zero. Otherwise -1.
 */
static inline int header_cmp(const char *expected, char *value, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        if (expected[i] != tolower(value[i])) {
            return -1;
        }
    }

    return 0;
}


static inline int method_lookup(struct mk_http_request *req,
                                struct mk_http_parser *p, char *buffer)
{
    int i;
    int len;

    /* Method lenght */
    len = field_len();

    /* Point the buffer */
    req->method = MK_METHOD_UNKNOWN;
    req->method_p.data = buffer + p->start;
    req->method_p.len  = len;

    for (i = 0; i < MK_METHOD_SIZEOF; i++) {
        if (len != mk_methods_table[i].len) {
            continue;
        }

        if (strncmp(buffer + p->start, mk_methods_table[i].name, len) == 0) {
            req->method = i;
            return i;
        }
    }

    return MK_METHOD_UNKNOWN;
}

static inline void request_set(mk_ptr_t *ptr, struct mk_http_parser *p, char *buffer)
{
    ptr->data = buffer + p->start;
    ptr->len  = field_len();
}

static inline int header_lookup(struct mk_http_parser *p, char *buffer)
{
    int i;
    int len;
    long val;
    char *endptr;

    struct mk_http_header *header;
    struct mk_http_header *header_extra;
    struct row_entry *h;

    len = (p->header_sep - p->header_key);

    for (i = p->header_min; i <= p->header_max; i++) {
        h = &mk_headers_table[i];

        /* Check string length first */
        if (h->len != len) {
            continue;
        }

        if (header_cmp(h->name + 1, buffer + p->header_key + 1, len - 1) == 0) {
            /* We got a header match, register the header index */
            header = &p->headers[i];
            header->type = i;
            header->key.data = buffer + p->header_key;
            header->key.len  = len;
            header->val.data = buffer + p->header_val;
            header->val.len  = p->end - p->header_val;

            if (i == MK_HEADER_HOST) {
                /* Handle a possible port number in the Host header */
                int sep = str_searchr(header->val.data, ':', header->val.len);
                if (sep > 0) {
                    int plen;
                    short int port_size = 6;
                    char port[port_size];

                    plen = header->val.len - sep - 1;
                    if (plen <= 0 || plen >= port_size) {
                        return -MK_CLIENT_BAD_REQUEST;
                    }
                    memcpy(&port, header->val.data + sep + 1, plen);
                    port[plen] = '\0';

                    val = strtol(port, &endptr, 10);
                    if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN))
                        || (errno != 0 && val == 0)) {
                        return -MK_CLIENT_BAD_REQUEST;
                    }

                    if (endptr == port || *endptr != '\0') {
                        return -MK_CLIENT_BAD_REQUEST;
                    }

                    p->header_host_port = val;

                    /* Re-set the Host header value without port */
                    header->val.len = sep;
                }
            }
            else if (i == MK_HEADER_CONTENT_LENGTH) {
                val = strtol(header->val.data, &endptr, 10);
                if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN))
                    || (errno != 0 && val == 0)) {
                    return -MK_CLIENT_REQUEST_ENTITY_TOO_LARGE;
                }
                if (endptr == header->val.data) {
                    return -1;
                }
                if (val < 0) {
                    return -1;
                }

                p->header_content_length = val;
            }
            else if (i == MK_HEADER_CONNECTION) {
                /* Check Connection: Keep-Alive */
                if (header->val.len == sizeof(MK_CONN_KEEP_ALIVE) - 1) {
                    if (header_cmp(MK_CONN_KEEP_ALIVE,
                                   header->val.data, header->val.len ) == 0) {
                        p->header_connection = MK_HTTP_PARSER_CONN_KA;
                    }
                }
                /* Check Connection: Close */
                else if (header->val.len == sizeof(MK_CONN_CLOSE) -1) {
                    if (header_cmp(MK_CONN_CLOSE,
                                   header->val.data, header->val.len) == 0) {
                        p->header_connection = MK_HTTP_PARSER_CONN_CLOSE;
                    }
                }
                else {
                    p->header_connection = MK_HTTP_PARSER_CONN_UNKNOWN;
                }
            }
            return 0;
        }
    }

    /*
     * The header_lookup did not match any known header, so we register this
     * entry into the headers_extra array.
     */
    if (p->headers_extra_count < MK_HEADER_EXTRA_SIZE) {
        header_extra = &p->headers_extra[p->headers_extra_count];
        header_extra->key.data = buffer + p->header_key;
        header_extra->key.len  = len;
        header_extra->val.data = buffer + p->header_val;
        header_extra->val.len  = p->end - p->header_val;
        p->headers_extra_count++;
        return 0;
    }

    return -1;
}

/*
 * This function is invoked everytime the parser evaluate the request is
 * OK. Here we perform some extra validations mostly based on some logic
 * and protocol requirements according to the data received.
 */
static inline int mk_http_parser_ok(struct mk_http_request *req,
                                    struct mk_http_parser *p) {

    /* Validate HTTP Version */
    if (req->protocol == MK_HTTP_PROTOCOL_UNKNOWN) {
        mk_http_error(MK_SERVER_HTTP_VERSION_UNSUP, req->session, req);
        return MK_HTTP_PARSER_ERROR;
    }

    /* POST checks */
    if (req->method == MK_METHOD_POST || req->method == MK_METHOD_PUT) {
        /* validate Content-Length exists */
        if (p->headers[MK_HEADER_CONTENT_LENGTH].type == 0) {
            mk_http_error(MK_CLIENT_LENGTH_REQUIRED, req->session, req);
            return MK_HTTP_PARSER_ERROR;
        }
    }

    return MK_HTTP_PARSER_OK;
}

/*
 * Parse the protocol and point relevant fields, don't take logic decisions
 * based on this, just parse to locate things.
 */
int mk_http_parser(struct mk_http_request *req, struct mk_http_parser *p,
                   char *buffer, int len)
{
    int i;
    int ret;

    for (i = p->i; i < len; p->i++, p->chars++, i++) {
        /* FIRST LINE LEVEL: Method, URI & Protocol */
        if (p->level == REQ_LEVEL_FIRST) {
            switch (p->status) {
            case MK_ST_REQ_METHOD:                      /* HTTP Method */
                if (buffer[i] == ' ') {
                    mark_end();
                    p->status = MK_ST_REQ_URI;
                    if (p->end < 2) {
                        return MK_HTTP_PARSER_ERROR;
                    }
                    method_lookup(req, p, buffer);
                    parse_next();
                }
                break;
            case MK_ST_REQ_URI:                         /* URI */
                if (buffer[i] == ' ') {
                    mark_end();
                    p->status = MK_ST_REQ_PROT_VERSION;
                    if (field_len() < 1) {
                        return MK_HTTP_PARSER_ERROR;
                    }
                    request_set(&req->uri, p, buffer);
                    parse_next();
                    continue;
                }
                else if (buffer[i] == '?') {
                    mark_end();
                    request_set(&req->uri, p, buffer);
                    p->status = MK_ST_REQ_QUERY_STRING;
                    parse_next();
                }
                else if (buffer[i] == '\r' || buffer[i] == '\n') {
                    mk_http_error(MK_CLIENT_BAD_REQUEST, req->session, req);
                    return MK_HTTP_PARSER_ERROR;
                }
                break;
            case MK_ST_REQ_QUERY_STRING:                /* Query string */
                if (buffer[i] == ' ') {
                    mark_end();
                    request_set(&req->query_string, p, buffer);
                    p->status = MK_ST_REQ_PROT_VERSION;
                    parse_next();
                }
                else if (buffer[i] == '\r' || buffer[i] == '\n') {
                    mk_http_error(MK_CLIENT_BAD_REQUEST, req->session, req);
                    return MK_HTTP_PARSER_ERROR;
                }
                break;
            case MK_ST_REQ_PROT_VERSION:                /* Protocol Version */
                if (buffer[i] == '\r') {
                    mark_end();
                    if (field_len() != 8) {
                        mk_http_error(MK_SERVER_HTTP_VERSION_UNSUP, req->session, req);
                        return MK_HTTP_PARSER_ERROR;
                    }

                    if (strncmp(buffer + p->start, "HTTP/1.", 7) != 0) {
                        mk_http_error(MK_SERVER_HTTP_VERSION_UNSUP, req->session, req);
                        return MK_HTTP_PARSER_ERROR;
                    }

                    request_set(&req->protocol_p, p, buffer);
                    if (req->protocol_p.data[req->protocol_p.len - 1] == '1') {
                        req->protocol = MK_HTTP_PROTOCOL_11;
                    }
                    else if (req->protocol_p.data[req->protocol_p.len - 1] == '0') {
                        req->protocol = MK_HTTP_PROTOCOL_10;
                    }
                    else {
                        req->protocol = MK_HTTP_PROTOCOL_UNKNOWN;
                    }

                    p->status = MK_ST_FIRST_FINALIZING;
                    continue;
                }
                break;
            case MK_ST_FIRST_FINALIZING:                  /* New Line */
                if (buffer[i] == '\n') {
                    p->level = REQ_LEVEL_CONTINUE;
                    parse_next();
                }
                else {
                    return MK_HTTP_PARSER_ERROR;
                }
                break;
            case MK_ST_BLOCK_END:
                if (buffer[i] == '\n') {
                    return mk_http_parser_ok(req, p);
                }
                else {
                    return MK_HTTP_PARSER_ERROR;
                }
                break;
            };
        }
        else if (p->level == REQ_LEVEL_CONTINUE) {
            if (buffer[i] == '\r') {
                p->level  = REQ_LEVEL_FIRST;
                p->status = MK_ST_BLOCK_END;
                continue;
            }
            else {
                p->level  = REQ_LEVEL_HEADERS;
                p->status = MK_ST_HEADER_KEY;
                p->chars  = 0;
            }
        }
        /* HEADERS: all headers stuff */
        if (p->level == REQ_LEVEL_HEADERS) {
            /* Expect a Header key */
            if (p->status == MK_ST_HEADER_KEY) {
                if (buffer[i] == '\r') {
                    if (p->chars == 0) {
                        p->level = REQ_LEVEL_END;
                        parse_next();
                    }
                    else {
                        return MK_HTTP_PARSER_ERROR;
                    }
                }

                if (p->chars == 0) {
                    /*
                     * We reach the start of a Header row, lets catch the most
                     * probable header. Note that we don't accept headers starting
                     * in lowercase.
                     *
                     * The goal of this 'first row character lookup', is to define a
                     * small range set of probable headers comparison once we catch
                     * a header end.
                     */
                    int s = tolower(buffer[i]);
                    switch (s) {
                    case 'a':
                        p->header_min = MK_HEADER_ACCEPT;
                        p->header_max = MK_HEADER_AUTHORIZATION;
                        break;
                    case 'c':
                        p->header_min = MK_HEADER_COOKIE;
                        p->header_max = MK_HEADER_CONTENT_TYPE;
                        break;
                    case 'i':
                        header_scope_eq(p, MK_HEADER_IF_MODIFIED_SINCE);
                        break;
                    case 'h':
                        header_scope_eq(p, MK_HEADER_HOST);
                        break;
                    case 'l':
                        p->header_min = MK_HEADER_LAST_MODIFIED;
                        p->header_max = MK_HEADER_LAST_MODIFIED_SINCE;
                        break;
                    case 'r':
                        p->header_min = MK_HEADER_REFERER;
                        p->header_max = MK_HEADER_RANGE;
                        break;
                    case 'u':
                        header_scope_eq(p, MK_HEADER_USER_AGENT);
                        break;
                    default:
                        p->header_key = -1;
                        p->header_sep = -1;
                        p->header_min = -1;
                        p->header_max = -1;
                    };
                    p->header_key = i;
                }

                /* Found key/value separator */
                if (buffer[i] == ':') {

                    /* Set the key/value middle point */
                    p->header_sep = i;

                    /* validate length */
                    mark_end();
                    if (field_len() < 1) {
                        return MK_HTTP_PARSER_ERROR;
                    }

                    /* Wait for a value */
                    p->status = MK_ST_HEADER_VALUE;
                    parse_next();
                }
            }
            /* Parsing the header value */
            else if (p->status == MK_ST_HEADER_VALUE) {
                /* Trim left, set starts only when found something != ' ' */
                if (buffer[i] == '\r' || buffer[i] == '\n') {
                    return MK_HTTP_PARSER_ERROR;
                }
                else if (buffer[i] != ' ') {
                    p->status = MK_ST_HEADER_VAL_STARTS;
                    p->start = p->header_val = i;
                }
                continue;
            }
            /* New header row starts */
            else if (p->status == MK_ST_HEADER_VAL_STARTS) {
                /* Maybe there is no more headers and we reach the end ? */
                if (buffer[i] == '\r') {
                    mark_end();
                    if (field_len() <= 0) {
                        return MK_HTTP_PARSER_ERROR;
                    }
                    p->status = MK_ST_HEADER_END;

                    /*
                     * A header row has ended, lets lookup the header and populate
                     * our headers table index.
                     */
                    ret = header_lookup(p, buffer);
                    if (ret != 0) {
                        if (ret < -1) {
                            mk_http_error(-ret, req->session, req);
                        }
                        return MK_HTTP_PARSER_ERROR;
                    }
                    parse_next();
                }
                else if (buffer[i] == '\n' && buffer[i - 1] != '\r') {
                    return MK_HTTP_PARSER_ERROR;
                }
                continue;
            }
            else if (p->status == MK_ST_HEADER_END) {
                if (buffer[i] == '\n') {
                    p->status = MK_ST_HEADER_KEY;
                    p->chars = -1;
                    parse_next();
                }
                else {
                    return MK_HTTP_PARSER_ERROR;
                }
            }
        }
        else if (p->level == REQ_LEVEL_END) {
            if (buffer[i] == '\n') {
                p->level = REQ_LEVEL_BODY;
                p->chars = -1;
                parse_next();
            }
            else {
                return MK_HTTP_PARSER_ERROR;
            }
        }
        else if (p->level == REQ_LEVEL_BODY) {
            /*
             * Reaching this level can means two things:
             *
             * - A Pipeline Request
             * - A Body content (POST/PUT methods
             */
            if (p->header_content_length > 0) {
                p->body_received += (len - i);

                if (p->body_received == p->header_content_length) {
                    return mk_http_parser_ok(req, p);
                }
                else {
                    return MK_HTTP_PARSER_PENDING;
                }
            }
            return mk_http_parser_ok(req, p);
        }
    }

    /*
     * FIXME: the code above needs to be handled in a different way
     */
    if (p->level == REQ_LEVEL_FIRST) {
        if (p->status == MK_ST_REQ_METHOD) {
            if (p->i > 10) {
                return MK_HTTP_PARSER_ERROR;
            }
            else {
                return MK_HTTP_PARSER_PENDING;
            }
        }

    }
    else if (p->level == REQ_LEVEL_HEADERS) {
        if (p->status == MK_ST_HEADER_KEY) {
            return MK_HTTP_PARSER_PENDING;
        }
        else if (p->status == MK_ST_HEADER_VALUE) {
            if (field_len() < 0) {
                return MK_HTTP_PARSER_PENDING;
            }
        }
    }
    else if (p->level == REQ_LEVEL_BODY) {
        if (p->header_content_length > 0) {
            p->body_received += (len - i);
            if (p->header_content_length == p->body_received) {
                return mk_http_parser_ok(req, p);
            }
            else {
                return MK_HTTP_PARSER_PENDING;
            }
        }
        if (p->header_content_length > 0 &&
            p->body_received <= p->header_content_length) {
            return MK_HTTP_PARSER_PENDING;
        }
        else if (p->chars == 0) {
            return mk_http_parser_ok(req, p);
        }
        else {
        }

    }

    return MK_HTTP_PARSER_PENDING;
}