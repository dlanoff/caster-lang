#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "h2o.h"
#if FLX_ENABLE_WEBSOCKET
#include "h2o/websocket.h"
#endif
#include <openssl/err.h>
#include <openssl/ssl.h>

#ifndef FLX_HOST_READ_HEADERS
#define FLX_HOST_READ_HEADERS 1
#endif

typedef struct {
    FLX_Task *task;
    FLX_STR method;
    FLX_STR url;
    FLX_STR body;
    pthread_t thread;
    bool thread_started;
    int status;
    char *response_body;
    size_t response_body_len;
    char error[256];
} FLX_HTTP_TASK;

static FLX_HTTP_TASK **flx_http_tasks = NULL;
static int64_t flx_http_task_count = 0;
static int64_t flx_http_task_cap = 0;

// Initializes request runtime state lazily, which keeps generated programs small because REQ is
// included only when used.
static void flx_req_runtime_init(void) {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
}

// Registers a hot HTTP task, which lets REQ.get start immediately because hold should wait on
// generic task state, not create work.
static void flx_http_scheduler_register(FLX_HTTP_TASK *http) {
    if (flx_http_task_count >= flx_http_task_cap) {
        flx_http_task_cap = flx_http_task_cap ? flx_http_task_cap * 2 : 8;
        flx_http_tasks = realloc(flx_http_tasks, sizeof(FLX_HTTP_TASK *) * (size_t)flx_http_task_cap);
        if (!flx_http_tasks) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
    }
    flx_http_tasks[flx_http_task_count++] = http;
}

// Bridges C strings into FLX_STR, which keeps adapter code isolated because generated C uses
// Caster string values.
static FLX_STR flx_req_str_from_c(const char *text) {
    size_t len = text ? strlen(text) : 0;
    FLX_STR out = flx_str_alloc((int64_t)len);
    if (len) memcpy(out.data, text, len);
    return out;
}

// Bridges C strings into FLX_STR, which keeps adapter code isolated because generated C uses
// Caster string values.
static FLX_STR flx_req_str_from_bytes(const char *data, size_t len) {
    FLX_STR out = flx_str_alloc((int64_t)len);
    if (len) memcpy(out.data, data, len);
    return out;
}

// Copies FLX_STR into a null-terminated C string, which is needed because OS, sockets, H2O, and
// sqlite expect C APIs.
static char *flx_req_cstr(FLX_STR text) {
    char *out = malloc((size_t)text.len + 1);
    if (!out) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
    if (text.len) memcpy(out, text.data, (size_t)text.len);
    out[text.len] = 0;
    return out;
}

// Bridges C strings into FLX_STR, which keeps adapter code isolated because generated C uses
// Caster string values.
static char *flx_req_strdup(const char *text) {
    size_t len = text ? strlen(text) : 0;
    char *out = malloc(len + 1);
    if (!out) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
    if (len) memcpy(out, text, len);
    out[len] = 0;
    return out;
}

// Stores HTTP task failure text, which keeps errors attached to the task because hold reads only
// generic completion state.
static void flx_http_task_set_error(FLX_HTTP_TASK *http, const char *message) {
    if (!http || http->error[0]) return;
    snprintf(http->error, sizeof(http->error), "%s", message ? message : "request failed");
}

// Appends response bytes into task storage, which handles incremental socket reads because HTTP
// bodies arrive in chunks.
static int flx_http_task_append_body(FLX_HTTP_TASK *http, const char *data, size_t len) {
    if (!len) return 0;
    char *next = realloc(http->response_body, http->response_body_len + len + 1);
    if (!next) {
        flx_http_task_set_error(http, "response body allocation failed");
        return -1;
    }
    http->response_body = next;
    memcpy(http->response_body + http->response_body_len, data, len);
    http->response_body_len += len;
    http->response_body[http->response_body_len] = 0;
    return 0;
}

// Finalizes an HTTP task result, which transitions generic task state because typed hold helpers
// need a stable result pointer.
static void flx_http_task_finish(FLX_HTTP_TASK *http) {
    if (!http || !http->task || http->task->status == FLX_TASK_DONE || http->task->status == FLX_TASK_CANCELLED) return;

    FLX_HttpRes *result = malloc(sizeof(FLX_HttpRes));
    if (!result) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
    result->status = http->error[0] ? 0 : http->status;
    result->body = http->error[0]
        ? flx_req_str_from_c(http->error)
        : flx_req_str_from_bytes(http->response_body ? http->response_body : "", http->response_body_len);
    result->headers.raw = NULL;
    http->task->result = result;
    http->task->status = FLX_TASK_DONE;
}

typedef struct {
    char *scheme;
    char *host;
    char *port;
    char *path;
    bool https;
} FLX_URL_PARTS;

// Releases parsed URL pieces, which keeps request cleanup local because URL parsing allocates per
// task.
static void flx_url_parts_drop(FLX_URL_PARTS *parts) {
    if (!parts) return;
    free(parts->scheme);
    free(parts->host);
    free(parts->port);
    free(parts->path);
    memset(parts, 0, sizeof(*parts));
}

// Parses a URL into transport pieces, which lets REQ share one request path because HTTP and HTTPS
// differ only after parsing.
static bool flx_parse_url(const char *url, FLX_URL_PARTS *parts, char *error, size_t error_len) {
    memset(parts, 0, sizeof(*parts));
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) {
        snprintf(error, error_len, "invalid URL");
        return false;
    }

    size_t scheme_len = (size_t)(scheme_end - url);
    parts->scheme = malloc(scheme_len + 1);
    if (!parts->scheme) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
    memcpy(parts->scheme, url, scheme_len);
    parts->scheme[scheme_len] = 0;

    parts->https = strcmp(parts->scheme, "https") == 0;
    if (!parts->https && strcmp(parts->scheme, "http") != 0) {
        snprintf(error, error_len, "REQ supports http and https URLs");
        return false;
    }

    const char *authority = scheme_end + 3;
    const char *path = strchr(authority, '/');
    size_t authority_len = path ? (size_t)(path - authority) : strlen(authority);
    if (authority_len == 0) {
        snprintf(error, error_len, "invalid URL host");
        return false;
    }

    const char *port_sep = memchr(authority, ':', authority_len);
    size_t host_len = port_sep ? (size_t)(port_sep - authority) : authority_len;
    size_t port_len = port_sep ? authority_len - host_len - 1 : 0;
    parts->host = malloc(host_len + 1);
    if (!parts->host) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
    memcpy(parts->host, authority, host_len);
    parts->host[host_len] = 0;

    if (port_sep) {
        parts->port = malloc(port_len + 1);
        if (!parts->port) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
        memcpy(parts->port, port_sep + 1, port_len);
        parts->port[port_len] = 0;
    } else {
        parts->port = flx_req_strdup(parts->https ? "443" : "80");
    }

    parts->path = flx_req_strdup(path && *path ? path : "/");
    return true;
}

// Opens the TCP connection for a client request, which isolates socket setup because higher HTTP
// code should not manage addrinfo details.
static int flx_connect_tcp(const char *host, const char *port, char *error, size_t error_len) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(host, port, &hints, &result);
    if (gai != 0) {
        snprintf(error, error_len, "DNS failed: %s", gai_strerror(gai));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *rp = result; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);

    if (fd < 0) snprintf(error, error_len, "connection failed");
    return fd;
}

// Writes the entire request buffer, which handles short socket writes because network calls are
// not guaranteed to flush all bytes at once.
static int flx_socket_write_all(int fd, SSL *ssl, const char *data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        int wrote = ssl ? SSL_write(ssl, data + offset, (int)(len - offset)) : (int)send(fd, data + offset, len - offset, 0);
        if (wrote <= 0) return -1;
        offset += (size_t)wrote;
    }
    return 0;
}

// Reads one response chunk, which hides TLS vs plain sockets because the parser only needs bytes.
static int flx_socket_read_some(int fd, SSL *ssl, char *buffer, size_t cap) {
    return ssl ? SSL_read(ssl, buffer, (int)cap) : (int)recv(fd, buffer, cap, 0);
}

// Builds the outbound HTTP/1 request, which keeps client formatting in one place because methods,
// path, host, and body must stay consistent.
static char *flx_http_build_request(FLX_HTTP_TASK *http, FLX_URL_PARTS *url, size_t *out_len) {
    char *method = flx_req_cstr(http->method);
    char *body = flx_req_cstr(http->body);
    size_t body_len = (size_t)http->body.len;
    bool has_body = body_len > 0;
    const char *body_headers = has_body ? "Content-Type: application/json\r\n" : "";
    char content_length[64] = "";
    if (has_body) snprintf(content_length, sizeof(content_length), "Content-Length: %zu\r\n", body_len);

    size_t len = strlen(method) + strlen(url->path) + strlen(url->host) + strlen(body_headers) + strlen(content_length) + body_len + 128;
    char *request = malloc(len);
    if (!request) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
    int n = snprintf(request, len,
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: Caster\r\n"
        "%s%s"
        "Connection: close\r\n"
        "\r\n",
        method, url->path, url->host, body_headers, content_length);
    if (n < 0 || (size_t)n >= len) {
        free(method);
        free(body);
        free(request);
        return NULL;
    }
    if (body_len) memcpy(request + n, body, body_len);
    *out_len = (size_t)n + body_len;
    free(method);
    free(body);
    return request;
}

// Parses the HTTP response into HttpRes, which turns socket bytes into typed Caster data because
// users should not inspect protocol text.
static int flx_http_parse_response(FLX_HTTP_TASK *http, char *response, size_t response_len) {
    char *body = NULL;
    for (size_t i = 3; i < response_len; i++) {
        if (response[i - 3] == '\r' && response[i - 2] == '\n' && response[i - 1] == '\r' && response[i] == '\n') {
            body = response + i + 1;
            break;
        }
    }
    if (!body) {
        flx_http_task_set_error(http, "invalid HTTP response");
        return -1;
    }

    int status = 0;
    if (sscanf(response, "HTTP/%*d.%*d %d", &status) != 1) {
        flx_http_task_set_error(http, "invalid HTTP status");
        return -1;
    }
    http->status = status;

    size_t body_len = response_len - (size_t)(body - response);
    return flx_http_task_append_body(http, body, body_len);
}

// Runs blocking client I/O off the caller path, which makes REQ tasks hot because request creation
// should begin work immediately.
static void *flx_http_task_thread(void *data) {
    FLX_HTTP_TASK *http = data;
    FLX_URL_PARTS url;
    char error[256] = "";
    int fd = -1;
    SSL_CTX *ssl_ctx = NULL;
    SSL *ssl = NULL;
    char *request = NULL;
    char *response = NULL;
    size_t response_len = 0;

    char *url_c = flx_req_cstr(http->url);
    if (!flx_parse_url(url_c, &url, error, sizeof(error))) {
        flx_http_task_set_error(http, error);
        goto done;
    }

    fd = flx_connect_tcp(url.host, url.port, error, sizeof(error));
    if (fd < 0) {
        flx_http_task_set_error(http, error);
        goto done;
    }

    if (url.https) {
        ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx) {
            flx_http_task_set_error(http, "TLS initialization failed");
            goto done;
        }
        SSL_CTX_set_default_verify_paths(ssl_ctx);
        ssl = SSL_new(ssl_ctx);
        if (!ssl) {
            flx_http_task_set_error(http, "TLS session allocation failed");
            goto done;
        }
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, url.host);
        SSL_set1_host(ssl, url.host);
        if (SSL_connect(ssl) != 1) {
            flx_http_task_set_error(http, "TLS connection failed");
            goto done;
        }
    }

    size_t request_len = 0;
    request = flx_http_build_request(http, &url, &request_len);
    if (!request) {
        flx_http_task_set_error(http, "request build failed");
        goto done;
    }
    if (flx_socket_write_all(fd, ssl, request, request_len) != 0) {
        flx_http_task_set_error(http, "request write failed");
        goto done;
    }

    char buffer[8192];
    for (;;) {
        int n = flx_socket_read_some(fd, ssl, buffer, sizeof(buffer));
        if (n < 0) {
            flx_http_task_set_error(http, "response read failed");
            goto done;
        }
        if (n == 0) break;
        char *next = realloc(response, response_len + (size_t)n + 1);
        if (!next) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
        response = next;
        memcpy(response + response_len, buffer, (size_t)n);
        response_len += (size_t)n;
        response[response_len] = 0;
    }

    if (flx_http_parse_response(http, response ? response : "", response_len) != 0) goto done;

done:
    flx_http_task_finish(http);
    if (ssl) SSL_free(ssl);
    if (ssl_ctx) SSL_CTX_free(ssl_ctx);
    if (fd >= 0) close(fd);
    free(request);
    free(response);
    free(url_c);
    flx_url_parts_drop(&url);
    return NULL;
}

// Starts a generic task as HTTP work, which preserves the task abstraction because hold should not
// know about request internals.
static void flx_http_task_start(FLX_Task *task) {
    if (!task || task->status != FLX_TASK_PENDING) return;
    flx_req_runtime_init();

    FLX_HTTP_TASK *http = (FLX_HTTP_TASK *)task->impl;
    task->status = FLX_TASK_RUNNING;
    if (pthread_create(&http->thread, NULL, flx_http_task_thread, http) != 0) {
        flx_http_task_set_error(http, "request thread creation failed");
        flx_http_task_finish(http);
        return;
    }
    http->thread_started = true;
}

// Destroys HTTP task storage, which keeps task cleanup behind the generic callback because task
// users only hold FLX_Task pointers.
static void flx_http_task_destroy(FLX_Task *task) {
    if (!task) return;
    FLX_HTTP_TASK *http = (FLX_HTTP_TASK *)task->impl;
    if (http) {
        if (http->thread_started) pthread_join(http->thread, NULL);
        free(http->response_body);
        free(http);
    }
    free(task->result);
    free(task);
}

// Advances pending runtime work, which gives hold a generic wait hook because future task sources
// should share one scheduler surface.
static void flx_scheduler_tick(void) {
    usleep(1000);
}

// Creates the typed HTTP task, which powers get/post/put/delete because each method differs mostly
// by method and body.
static FLX_UNUSED FLX_TSK_HTTPRES REQ_request(FLX_STR method, FLX_STR url, FLX_STR body) {
    FLX_HTTP_TASK *http = malloc(sizeof(FLX_HTTP_TASK));
    if (!http) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
    memset(http, 0, sizeof(FLX_HTTP_TASK));
    http->method = method;
    http->url = url;
    http->body = body;

    FLX_Task *base = malloc(sizeof(FLX_Task));
    if (!base) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
    memset(base, 0, sizeof(FLX_Task));
    base->status = FLX_TASK_PENDING;
    base->impl = http;
    base->start = flx_http_task_start;
    base->destroy = flx_http_task_destroy;
    http->task = base;

    flx_http_scheduler_register(http);
    base->start(base);

    FLX_TSK_HTTPRES task;
    task.task = base;
    return task;
}

// Creates a GET task, which keeps the user API small because the shared request helper owns
// transport setup.
static FLX_UNUSED FLX_TSK_HTTPRES REQ_get(FLX_STR url) {
    return REQ_request(flx_str_lit("GET", 3), url, flx_str_lit("", 0));
}

// Creates a DELETE task, which keeps the user API small because the shared request helper owns
// transport setup.
static FLX_UNUSED FLX_TSK_HTTPRES REQ_delete(FLX_STR url) {
    return REQ_request(flx_str_lit("DELETE", 6), url, flx_str_lit("", 0));
}

// Creates a POST task, which keeps the user API small because the shared request helper owns
// transport setup.
static FLX_UNUSED FLX_TSK_HTTPRES REQ_post(FLX_STR url, FLX_STR body) {
    return REQ_request(flx_str_lit("POST", 4), url, body);
}

// Creates a PUT task, which keeps the user API small because the shared request helper owns
// transport setup.
static FLX_UNUSED FLX_TSK_HTTPRES REQ_put(FLX_STR url, FLX_STR body) {
    return REQ_request(flx_str_lit("PUT", 3), url, body);
}

typedef FLX_HttpRes (*FLX_HostHandler)(FLX_HttpReq req);
typedef FLX_HttpRes (*FLX_HostRefHandler)(FLX_HttpReq *req);
typedef FLX_HttpRes (*FLX_HostCtxHandler)(void *ctx, FLX_HttpReq req);
typedef FLX_HttpRes (*FLX_HostCtxRefHandler)(void *ctx, FLX_HttpReq *req);
typedef FLX_STR (*FLX_WsHandler)(FLX_STR message);

typedef struct {
    FLX_HostHandler handler;
    FLX_HostRefHandler ref_handler;
    FLX_HostCtxHandler ctx_handler;
    FLX_HostCtxRefHandler ctx_ref_handler;
    FLX_WsHandler ws_handler;
    void *ctx_value;
    int port;
    h2o_globalconf_t config;
    h2o_context_t ctx;
    h2o_accept_ctx_t accept_ctx;
    SSL_CTX *ssl_ctx;
} FLX_HOST;

typedef struct {
    h2o_handler_t super;
    FLX_HOST *host;
} FLX_H2O_HANDLER;

static FLX_HOST *flx_active_host = NULL;

// Maps status codes to reason phrases, which keeps hosted responses valid because H2O expects
// status metadata alongside the body.
static const char *flx_http_reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default: return "OK";
    }
}

#if FLX_HOST_READ_HEADERS
// Adds one request header to a Caster map, which exposes headers as normal data because handler
// code should use MAP operations.
static FLX_MAP_STR_STR_ flx_host_header_add(FLX_MAP_STR_STR_ headers, const char *name, size_t name_len, const char *value, size_t value_len) {
    FLX_STR header_name = flx_req_str_from_bytes(name ? name : "", name_len);
    FLX_STR header_value = flx_req_str_from_bytes(value ? value : "", value_len);
    flx_map_str_str__set_copy(&headers, header_name, header_value);
    flx_drop_str(header_name);
    flx_drop_str(header_value);
    return headers;
}

// Copies H2O request headers into Caster data, which separates handler lifetimes from H2O pools
// because user code sees owned values.
static FLX_MAP_STR_STR_ flx_host_read_headers(h2o_req_t *req) {
    FLX_MAP_STR_STR_ headers = flx_map_str_str__make(NULL, 0);
    if (req->authority.len) {
        headers = flx_host_header_add(headers, "host", 4, req->authority.base, req->authority.len);
    }
    for (size_t i = 0; i < req->headers.size; i++) {
        h2o_header_t *header = &req->headers.entries[i];
        const char *name = header->orig_name ? header->orig_name : header->name->base;
        headers = flx_host_header_add(headers, name, header->name->len, header->value.base, header->value.len);
    }
    return headers;
}
#endif

// Copies Caster response headers back to H2O, which preserves WEB.json/text behavior because
// handlers return ordinary response maps.
static void flx_host_add_response_headers(h2o_req_t *req, FLX_HttpRes *res) {
    FLX_STR content_type_key = flx_str_lit("content-type", 12);
    bool wrote_content_type = false;

    size_t iter = 0;
    void *item = NULL;
    while (res->headers.raw && hashmap_iter(res->headers.raw, &iter, &item)) {
        FLX_MAP_STR_STR__ENTRY *entry = item;
        if (flx_str_eq(entry->key, flx_str_lit("content-length", 14))) continue;

        if (flx_str_eq(entry->key, content_type_key)) {
            h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, entry->value.data, (size_t)entry->value.len);
            wrote_content_type = true;
            continue;
        }

        h2o_add_header_by_str(&req->pool, &req->res.headers,
            entry->key.data, (size_t)entry->key.len, 0, NULL,
            entry->value.data, (size_t)entry->value.len);
    }

    if (!wrote_content_type) {
        h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("text/plain"));
    }
}

// Dispatches an H2O request into a Caster handler, which is the adapter seam because host
// libraries should not leak into generated handler signatures.
static int flx_host_dispatch(FLX_HOST *host, h2o_req_t *hreq) {
    if (!host || (!host->handler && !host->ref_handler && !host->ctx_handler && !host->ctx_ref_handler)) return -1;

    FLX_HttpReq req;
    memset(&req, 0, sizeof(req));
    req.method = flx_req_str_from_bytes(hreq->method.base, hreq->method.len);
    req.path = flx_req_str_from_bytes(hreq->path.base, hreq->path.len);
    req.body = flx_req_str_from_bytes(hreq->entity.base ? hreq->entity.base : "", hreq->entity.len);
#if FLX_HOST_READ_HEADERS
    req.headers = flx_host_read_headers(hreq);
#else
    req.headers = (FLX_MAP_STR_STR_){0};
#endif
    req.params.raw = NULL;
    req.query.raw = NULL;
    req.ctx.raw = NULL;
    req.res = (FLX_HttpRes){0};

    FLX_HttpRes res;
    if (host->ctx_ref_handler) res = host->ctx_ref_handler(host->ctx_value, &req);
    else if (host->ctx_handler) res = host->ctx_handler(host->ctx_value, req);
    else if (host->ref_handler) res = host->ref_handler(&req);
    else res = host->handler(req);

    int status = res.status ? (int)res.status : 200;
    hreq->res.status = status;
    hreq->res.reason = flx_http_reason(status);
    hreq->res.content_length = res.body.len > 0 ? (size_t)res.body.len : 0;
    flx_host_add_response_headers(hreq, &res);
    h2o_send_inline(hreq, res.body.data ? res.body.data : "", res.body.len > 0 ? (size_t)res.body.len : 0);

    req.res = (FLX_HttpRes){0};
    flx_drop_httpreq(req);
    flx_drop_httpres(res);
    return 0;
}

#if FLX_ENABLE_WEBSOCKET
// Dispatches websocket messages into Caster callbacks, which keeps websocket handling typed
// because H2O/wslay events are library-specific.
static void flx_ws_on_message(h2o_websocket_conn_t *conn, const struct wslay_event_on_msg_recv_arg *arg) {
    FLX_HOST *host = conn ? (FLX_HOST *)conn->data : NULL;
    if (!conn || !host || !host->ws_handler || arg == NULL) {
        if (conn) h2o_websocket_close(conn);
        return;
    }

    if (wslay_is_ctrl_frame(arg->opcode)) return;

    FLX_STR message = flx_req_str_from_bytes((const char *)arg->msg, arg->msg_length);
    FLX_STR response = host->ws_handler(message);
    if (response.len > 0) {
        struct wslay_event_msg msgarg = {
            arg->opcode,
            (const uint8_t *)response.data,
            (size_t)response.len
        };
        wslay_event_queue_msg(conn->ws_ctx, &msgarg);
        h2o_websocket_proceed(conn);
    }
    flx_drop_str(message);
    flx_drop_str(response);
}

// Attempts websocket upgrade handling, which keeps HTTP routing fast because upgrade requests can
// exit before normal dispatch.
static bool flx_host_try_websocket(FLX_HOST *host, h2o_req_t *req) {
    if (!host || !host->ws_handler) return false;
    const char *client_key = NULL;
    if (h2o_is_websocket_handshake(req, &client_key) != 0 || client_key == NULL) return false;
    h2o_upgrade_to_websocket(req, client_key, host, flx_ws_on_message);
    return true;
}
#else
// Attempts websocket upgrade handling, which keeps HTTP routing fast because upgrade requests can
// exit before normal dispatch.
static bool flx_host_try_websocket(FLX_HOST *host, h2o_req_t *req) {
    (void)host;
    (void)req;
    return false;
}
#endif

// Receives H2O HTTP callbacks, which forwards to Caster dispatch because the rest of the runtime
// should stay host-library neutral.
static int flx_h2o_on_req(h2o_handler_t *self, h2o_req_t *req) {
    FLX_H2O_HANDLER *handler = (FLX_H2O_HANDLER *)self;
    if (flx_host_try_websocket(handler->host, req)) return 0;
    if (handler->host && handler->host->ws_handler) {
        req->res.status = 404;
        req->res.reason = "Not Found";
        req->res.content_length = 9;
        h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("text/plain"));
        h2o_send_inline(req, H2O_STRLIT("not found"));
        return 0;
    }
    return flx_host_dispatch(handler->host, req);
}

// Accepts H2O sockets, which wires new connections into the host context because H2O owns
// per-connection event state.
static void flx_h2o_on_accept(h2o_socket_t *listener, const char *err) {
    if (err != NULL || !flx_active_host) return;

    h2o_socket_t *sock = h2o_evloop_socket_accept(listener);
    if (!sock) return;
    h2o_accept(&flx_active_host->accept_ctx, sock);
}

// Creates the listening socket, which keeps host startup explicit because the runtime controls
// port binding and reuse options.
static int flx_h2o_create_listener(FLX_HOST *host) {
    struct sockaddr_in addr;
    int fd = -1;
    int reuseaddr_flag = 1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)host->port);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) return -1;

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_flag, sizeof(reuseaddr_flag)) != 0 ||
        bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, SOMAXCONN) != 0) {
        close(fd);
        return -1;
    }

    h2o_socket_t *sock = h2o_evloop_socket_create(host->ctx.loop, fd, H2O_SOCKET_FLAG_DONT_READ);
    h2o_socket_read_start(sock, flx_h2o_on_accept);
    return 0;
}

// Configures TLS for H2O, which keeps HTTPS on the same host path because only the accept context
// changes.
static int flx_h2o_setup_tls(FLX_HOST *host, FLX_STR cert_file, FLX_STR key_file) {
    char *cert = flx_req_cstr(cert_file);
    char *key = flx_req_cstr(key_file);

    host->ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (!host->ssl_ctx) {
        fprintf(stderr, "caster: REQ.hostTLS failed to initialize TLS\n");
        free(cert);
        free(key);
        return -1;
    }
    SSL_CTX_set_options(host->ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

    if (SSL_CTX_use_certificate_chain_file(host->ssl_ctx, cert) != 1) {
        fprintf(stderr, "caster: REQ.hostTLS failed to load certificate %s\n", cert);
        free(cert);
        free(key);
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(host->ssl_ctx, key, SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "caster: REQ.hostTLS failed to load private key %s\n", key);
        free(cert);
        free(key);
        return -1;
    }

    host->accept_ctx.ssl_ctx = host->ssl_ctx;
    free(cert);
    free(key);
    return 0;
}

// Runs the H2O event loop, which backs WEB/REQ hosting because generated programs need one
// blocking server entry point.
static void flx_h2o_host_run(FLX_HOST *host, bool tls, FLX_STR cert_file, FLX_STR key_file) {
    flx_req_runtime_init();

    h2o_config_init(&host->config);
    h2o_hostconf_t *hostconf = h2o_config_register_host(&host->config, h2o_iovec_init(H2O_STRLIT("default")), 65535);
    h2o_pathconf_t *pathconf = h2o_config_register_path(hostconf, "/", 0);
    FLX_H2O_HANDLER *handler = (FLX_H2O_HANDLER *)h2o_create_handler(pathconf, sizeof(*handler));
    handler->super.on_req = flx_h2o_on_req;
    handler->host = host;

    h2o_context_init(&host->ctx, h2o_evloop_create(), &host->config);
    host->accept_ctx.ctx = &host->ctx;
    host->accept_ctx.hosts = host->config.hosts;

    if (tls && flx_h2o_setup_tls(host, cert_file, key_file) != 0) exit(1);

    flx_active_host = host;
    if (flx_h2o_create_listener(host) != 0) {
        fprintf(stderr, "caster: REQ.host failed to listen on port %d: %s\n", host->port, strerror(errno));
        exit(1);
    }

    while (h2o_evloop_run(host->ctx.loop, INT32_MAX) == 0) {
    }
}

// Exposes REQ.host to generated C, which preserves the Caster API shape because handler signatures
// vary by REF, context, TLS, or websocket mode.
static FLX_UNUSED void REQ_host(int64_t port, FLX_HostHandler handler) {
    FLX_HOST host;
    memset(&host, 0, sizeof(host));
    host.handler = handler;
    host.port = (int)port;
    flx_h2o_host_run(&host, false, flx_str_lit("", 0), flx_str_lit("", 0));
}

// Exposes REQ.host_ref to generated C, which preserves the Caster API shape because handler
// signatures vary by REF, context, TLS, or websocket mode.
static FLX_UNUSED void REQ_host_ref(int64_t port, FLX_HostRefHandler handler) {
    FLX_HOST host;
    memset(&host, 0, sizeof(host));
    host.ref_handler = handler;
    host.port = (int)port;
    flx_h2o_host_run(&host, false, flx_str_lit("", 0), flx_str_lit("", 0));
}

// Exposes REQ.host_ctx to generated C, which preserves the Caster API shape because handler
// signatures vary by REF, context, TLS, or websocket mode.
static FLX_UNUSED void REQ_host_ctx(int64_t port, void *ctx, FLX_HostCtxHandler handler) {
    FLX_HOST host;
    memset(&host, 0, sizeof(host));
    host.ctx_value = ctx;
    host.ctx_handler = handler;
    host.port = (int)port;
    flx_h2o_host_run(&host, false, flx_str_lit("", 0), flx_str_lit("", 0));
}

// Exposes REQ.host_ctx_ref to generated C, which preserves the Caster API shape because handler
// signatures vary by REF, context, TLS, or websocket mode.
static FLX_UNUSED void REQ_host_ctx_ref(int64_t port, void *ctx, FLX_HostCtxRefHandler handler) {
    FLX_HOST host;
    memset(&host, 0, sizeof(host));
    host.ctx_value = ctx;
    host.ctx_ref_handler = handler;
    host.port = (int)port;
    flx_h2o_host_run(&host, false, flx_str_lit("", 0), flx_str_lit("", 0));
}

// Exposes REQ.hostTLS to generated C, which preserves the Caster API shape because handler
// signatures vary by REF, context, TLS, or websocket mode.
static FLX_UNUSED void REQ_hostTLS(int64_t port, FLX_STR cert_file, FLX_STR key_file, FLX_HostHandler handler) {
    FLX_HOST host;
    memset(&host, 0, sizeof(host));
    host.handler = handler;
    host.port = (int)port;
    flx_h2o_host_run(&host, true, cert_file, key_file);
}

// Exposes REQ.hostTLS_ref to generated C, which preserves the Caster API shape because handler
// signatures vary by REF, context, TLS, or websocket mode.
static FLX_UNUSED void REQ_hostTLS_ref(int64_t port, FLX_STR cert_file, FLX_STR key_file, FLX_HostRefHandler handler) {
    FLX_HOST host;
    memset(&host, 0, sizeof(host));
    host.ref_handler = handler;
    host.port = (int)port;
    flx_h2o_host_run(&host, true, cert_file, key_file);
}

// Exposes REQ.hostTLS_ctx to generated C, which preserves the Caster API shape because handler
// signatures vary by REF, context, TLS, or websocket mode.
static FLX_UNUSED void REQ_hostTLS_ctx(int64_t port, FLX_STR cert_file, FLX_STR key_file, void *ctx, FLX_HostCtxHandler handler) {
    FLX_HOST host;
    memset(&host, 0, sizeof(host));
    host.ctx_value = ctx;
    host.ctx_handler = handler;
    host.port = (int)port;
    flx_h2o_host_run(&host, true, cert_file, key_file);
}

// Exposes REQ.hostTLS_ctx_ref to generated C, which preserves the Caster API shape because handler
// signatures vary by REF, context, TLS, or websocket mode.
static FLX_UNUSED void REQ_hostTLS_ctx_ref(int64_t port, FLX_STR cert_file, FLX_STR key_file, void *ctx, FLX_HostCtxRefHandler handler) {
    FLX_HOST host;
    memset(&host, 0, sizeof(host));
    host.ctx_value = ctx;
    host.ctx_ref_handler = handler;
    host.port = (int)port;
    flx_h2o_host_run(&host, true, cert_file, key_file);
}

// Reports missing websocket support, which fails clearly because the vendored H2O build may not
// include wslay.
static void flx_ws_unavailable(void) {
    fprintf(stderr, "caster: REQ.ws requires H2O built with wslay; install wslay and run `make -f build/Makefile h2o`\n");
    exit(1);
}

// Exposes REQ.ws to generated C, which preserves the Caster API shape because handler signatures
// vary by REF, context, TLS, or websocket mode.
static FLX_UNUSED void REQ_ws(int64_t port, FLX_WsHandler handler) {
#if FLX_ENABLE_WEBSOCKET
    FLX_HOST host;
    memset(&host, 0, sizeof(host));
    host.ws_handler = handler;
    host.port = (int)port;
    flx_h2o_host_run(&host, false, flx_str_lit("", 0), flx_str_lit("", 0));
#else
    (void)port;
    (void)handler;
    flx_ws_unavailable();
#endif
}

// Exposes REQ.wsTLS to generated C, which preserves the Caster API shape because handler
// signatures vary by REF, context, TLS, or websocket mode.
static FLX_UNUSED void REQ_wsTLS(int64_t port, FLX_STR cert_file, FLX_STR key_file, FLX_WsHandler handler) {
#if FLX_ENABLE_WEBSOCKET
    FLX_HOST host;
    memset(&host, 0, sizeof(host));
    host.ws_handler = handler;
    host.port = (int)port;
    flx_h2o_host_run(&host, true, cert_file, key_file);
#else
    (void)port;
    (void)cert_file;
    (void)key_file;
    (void)handler;
    flx_ws_unavailable();
#endif
}
