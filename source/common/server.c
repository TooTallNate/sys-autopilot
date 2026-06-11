#include "server.h"
#include "http.h"
#include "routes.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <switch.h>

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static int create_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblocking(fd);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((u16)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void handle_connection(int fd, const Config *cfg) {
    // Non-blocking I/O: the http layer waits via poll() with an inactivity
    // timeout, so a stalled client can't wedge the server, and the idle
    // callback keeps running during slow transfers.
    set_nonblocking(fd);

    static HttpRequest req; // single-threaded server; keep off the stack
    if (!http_read_request(fd, &req))
        return;

    if (config_auth_enabled(cfg) &&
        !http_check_basic_auth(&req, cfg->username, cfg->password)) {
        http_send_unauthorized(fd);
        return;
    }

    routes_handle(&req);
}

void server_run(const Config *cfg, ServerIdleCb idle) {
    int listen_fd = -1;

    // Let the http I/O layer drive the idle callback during transfers too.
    http_set_idle_callback(idle);

    for (;;) {
        if (listen_fd < 0) {
            listen_fd = create_listener(cfg->port);
            if (listen_fd < 0) {
                LOGF("server: bind/listen on port %d failed (errno=%d), retrying\n",
                     cfg->port, errno);
                if (idle && !idle())
                    return;
                svcSleepThread(1000000000LL); // 1s
                continue;
            }
            LOGF("server: listening on port %d\n", cfg->port);
        }

        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 100);

        if (idle && !idle())
            break;

        if (pr < 0) {
            // bsd service hiccup; rebuild the listener.
            LOGF("server: poll failed (errno=%d), rebuilding listener\n", errno);
            close(listen_fd);
            listen_fd = -1;
            svcSleepThread(1000000000LL);
            continue;
        }
        if (pr == 0 || !(pfd.revents & POLLIN))
            continue;

        int client = accept(listen_fd, NULL, NULL);
        if (client < 0) {
            // EAGAIN: spurious wakeup on the non-blocking listener.
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                LOGF("server: accept failed (errno=%d)\n", errno);
            continue;
        }

        handle_connection(client, cfg);
        close(client);
    }

    http_set_idle_callback(NULL);
    if (listen_fd >= 0)
        close(listen_fd);
}
