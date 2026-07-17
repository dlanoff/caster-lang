#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "h2o.h"

static h2o_globalconf_t config;
static h2o_context_t ctx;
static h2o_accept_ctx_t accept_ctx;

static int health_handler(h2o_handler_t *self, h2o_req_t *req) {
    (void)self;

    if (!h2o_memis(req->method.base, req->method.len, H2O_STRLIT("GET"))) return -1;

    static const char body[] = "{\"ok\":true,\"service\":\"caster-bank\",\"status\":\"healthy\"}";
    req->res.status = 200;
    req->res.reason = "OK";
    req->res.content_length = sizeof(body) - 1;
    h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("application/json"));
    h2o_send_inline(req, body, sizeof(body) - 1);
    return 0;
}

static int not_found_handler(h2o_handler_t *self, h2o_req_t *req) {
    (void)self;

    req->res.status = 404;
    req->res.reason = "Not Found";
    req->res.content_length = sizeof("not found\n") - 1;
    h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("text/plain"));
    h2o_send_inline(req, H2O_STRLIT("not found\n"));
    return 0;
}

static void register_handler(h2o_hostconf_t *hostconf, const char *path, int (*on_req)(h2o_handler_t *, h2o_req_t *)) {
    h2o_pathconf_t *pathconf = h2o_config_register_path(hostconf, path, 0);
    h2o_handler_t *handler = h2o_create_handler(pathconf, sizeof(*handler));
    handler->on_req = on_req;
}

static void on_accept(h2o_socket_t *listener, const char *err) {
    if (err != NULL) return;

    h2o_socket_t *sock = h2o_evloop_socket_accept(listener);
    if (sock == NULL) return;

    h2o_accept(&accept_ctx, sock);
}

static int parse_port(void) {
    const char *configured = getenv("BENCH_PORT");
    if (configured == NULL || configured[0] == 0) return 8000;

    char *end = NULL;
    long parsed = strtol(configured, &end, 10);
    if (end == configured || *end != 0 || parsed < 1 || parsed > 65535) {
        fprintf(stderr, "h2o-get: invalid BENCH_PORT=%s\n", configured);
        exit(2);
    }
    return (int)parsed;
}

static const char *bind_host(void) {
    const char *configured = getenv("BENCH_HOST");
    return configured && configured[0] ? configured : "127.0.0.1";
}

static int create_listener(const char *host, int port) {
    struct sockaddr_in addr;
    int fd = -1;
    int reuseaddr_flag = 1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "h2o-get: BENCH_HOST must be an IPv4 address, got %s\n", host);
        return -1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) return -1;

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_flag, sizeof(reuseaddr_flag)) != 0 ||
        bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, H2O_SOMAXCONN) != 0) {
        close(fd);
        return -1;
    }

    h2o_socket_t *sock = h2o_evloop_socket_create(ctx.loop, fd, H2O_SOCKET_FLAG_DONT_READ);
    h2o_socket_read_start(sock, on_accept);
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    h2o_config_init(&config);
    h2o_hostconf_t *hostconf = h2o_config_register_host(&config, h2o_iovec_init(H2O_STRLIT("default")), 65535);
    register_handler(hostconf, "/health", health_handler);
    register_handler(hostconf, "/", not_found_handler);

    h2o_context_init(&ctx, h2o_evloop_create(), &config);
    accept_ctx.ctx = &ctx;
    accept_ctx.hosts = config.hosts;

    const char *host = bind_host();
    int port = parse_port();
    if (create_listener(host, port) != 0) {
        fprintf(stderr, "h2o-get: failed to listen on %s:%d: %s\n", host, port, strerror(errno));
        return 1;
    }

    fprintf(stderr, "h2o-get: listening on %s:%d\n", host, port);
    while (h2o_evloop_run(ctx.loop, INT32_MAX) == 0) {
    }

    return 0;
}
