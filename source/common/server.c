#include "server.h"
#include "http.h"
#include "input.h"
#include "mdns.h"
#include "netif.h"
#include "oauth.h"
#include "power.h"
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
    if (listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// OAuth discovery, login, and token endpoints must be reachable without
// credentials (they ARE the way to obtain credentials). CORS preflights
// carry no Authorization header either.
static bool path_is_public(const HttpRequest *req) {
    return strncmp(req->path, "/.well-known/", 13) == 0 ||
           strncmp(req->path, "/oauth/", 7) == 0 ||
           strcmp(req->method, "OPTIONS") == 0;
}

// Drains any request body the handler didn't consume, so the next request on a
// kept-alive connection starts on a clean boundary. Returns false if the body
// is unexpectedly large/stalled (treat as non-reusable).
static bool drain_body(HttpRequest *req) {
    if (!req->has_content_length)
        return true;
    char scratch[1024];
    int guard = 0;
    while (req->body_consumed < req->content_length) {
        if (++guard > 4096) // ~4MB cap; runaway -> just close
            return false;
        if (http_read_body(req, scratch, sizeof(scratch)) <= 0)
            return false;
    }
    return true;
}

typedef enum { CONN_CLOSE, CONN_KEEP } ConnDisp;

// Handles one request. Returns CONN_KEEP if the connection may be reused
// (keep-alive, body drained) or CONN_CLOSE to close.
static ConnDisp handle_one(int fd, const Config *cfg) {
    static HttpRequest req; // single-threaded server; keep off the stack
    if (!http_read_request(fd, &req))
        return CONN_CLOSE;

    // Keep-alive for HTTP/1.1 so the MCP SDK can reuse one socket across
    // initialize -> notifications/initialized -> tools/list. The GET stream is
    // declined with 405+close, so nothing tries to pipeline onto it.
    req.keep_alive = req.http11 && !req.conn_close;
    http_set_keep_alive(req.keep_alive);

    if (config_auth_enabled(cfg) && !path_is_public(&req)) {
        bool basic_cfg = cfg->username[0] != '\0' && cfg->password[0] != '\0';
        bool ok = basic_cfg &&
                  http_check_basic_auth(&req, cfg->username, cfg->password);

        char bearer[160];
        if (!ok && http_get_bearer(&req, bearer, sizeof(bearer))) {
            // Static config token, or any OAuth-issued token from tokens.txt.
            if (cfg->token[0] != '\0' && http_secure_streq(bearer, cfg->token))
                ok = true;
            else if (oauth_token_valid(bearer))
                ok = true;
        }

        if (!ok) {
            // Bearer is always offered: OAuth-capable clients use the
            // resource_metadata hint to run the browser flow.
            http_send_unauthorized(&req, basic_cfg, true);
            return (req.keep_alive && drain_body(&req)) ? CONN_KEEP : CONN_CLOSE;
        }
    }

    routes_handle(&req);
    return (req.keep_alive && drain_body(&req)) ? CONN_KEEP : CONN_CLOSE;
}

// Serves an accepted connection (one or more keep-alive requests).
static void handle_connection(int fd, const Config *cfg) {
    // Non-blocking I/O: the http layer waits via poll() with an inactivity
    // timeout, so a stalled client can't wedge the server, and the idle
    // callback keeps running during slow transfers.
    set_nonblocking(fd);

    // Serve multiple requests on one connection (HTTP/1.1 keep-alive). Bounded
    // so a single client can't monopolize the single-threaded server; the next
    // read blocks on the 10s http I/O timeout, so an idle peer disconnects. MCP
    // clients reuse the socket across initialize -> initialized -> tools/list,
    // so closing after each response broke their transport.
    for (int served = 0; served < 64; served++) {
        if (handle_one(fd, cfg) != CONN_KEEP)
            break;
        // Keep-alive: only stay if the next request is already arriving. The
        // server is single-threaded, so blocking here on a quiet socket would
        // starve everyone else; if the peer isn't immediately pipelining, close
        // and let it reconnect (the response already advertised keep-alive, so
        // pooled clients just open a fresh socket).
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, 50) <= 0)
            break;
    }
}

// Sleeps ~1s in 100ms slices, invoking the idle callback each slice so the
// dev app stays responsive (input is sampled per idle call; a long sleep
// would make button presses easy to miss). Returns false on idle shutdown.
static bool retry_wait(ServerIdleCb idle) {
    for (int i = 0; i < 10; i++) {
        if (idle && !idle())
            return false;
        svcSleepThread(100000000LL); // 100ms
    }
    return true;
}

void server_run(const Config *cfg, ServerIdleCb idle) {
    int listen_fd = -1;
    int mdns_fd = -1;
    bool suspended = false;

    // Build the mDNS / DNS-SD advertising parameters once. If the local IP
    // isn't available yet (interface down), discovery is retried lazily each
    // time the listener is (re)opened below.
    static MdnsConfig mdns_cfg; // off the stack; lives for the loop
    bool mdns_ready = mdns_config_init(&mdns_cfg, cfg);
    if (!mdns_ready)
        LOGF("server: mDNS: local IP not yet known; will retry\n");

    // Pending unsolicited announcements. Set when the mDNS socket (re)opens
    // and decremented only when a send actually succeeds, so we keep retrying
    // across the seconds it can take for routing to come up after the network
    // changes (sends fail with EHOSTUNREACH until then) instead of giving up.
    int mdns_announce_left = 0;

    // Throttle for the periodic nifm IP-change check (the reliable backstop;
    // the connectivity event can fire before the address has settled or be
    // missed entirely). The loop spins ~every 100ms; check every ~2s.
    int netcheck_ticks = 0;

    // Let the http I/O layer drive the idle callback during transfers too.
    http_set_idle_callback(idle);

    for (;;) {
        // Participate in sleep/wake transitions: all sockets must be closed
        // and no bsd IPC may be issued between the sleep acknowledgement and
        // the wake notification, or bsdsockets aborts and the whole console
        // crashes on the next wake. Since this loop is the only thread doing
        // socket I/O, nothing is in flight when we acknowledge here.
        PowerEvent pe = power_poll();
        if (pe == PowerEvent_Sleep) {
            // Log BEFORE suspending the sink, then block all further file I/O:
            // no fsp-srv (or bsd) IPC may occur between this point and the wake
            // notification, or the console hangs on wake. The log sink writes
            // to the SD card, so it must stay silent across the whole window.
            LOGF("server: power: sleeping, releasing sockets + HDLS\n");
            log_set_suspended(true);
            if (listen_fd >= 0) {
                close(listen_fd);
                listen_fd = -1;
            }
            if (mdns_fd >= 0) {
                mdns_close(mdns_fd);
                mdns_fd = -1;
            }
            // Release the HDLS work buffer: holding hid transfer memory
            // across the transition crashes the sleep sequence.
            input_suspend();
            suspended = true;
            power_ack();
        } else if (pe == PowerEvent_Wake) {
            power_ack();
            suspended = false;
            // Safe to touch the SD card again now that we are awake.
            log_set_suspended(false);
            LOGF("server: power: awake\n");
        }
        if (suspended) {
            if (idle && !idle())
                break;
            svcSleepThread(100000000LL); // 100ms between power_poll checks
            continue;
        }

        // React to network connectivity changes (wifi connect/disconnect,
        // airplane mode, DHCP renewal) by polling nifm for our current IP every
        // ~2s and acting when it changes. (We tested nifm's connectivity event
        // on hardware; on an unsubmitted request it never fires, so polling is
        // the actual working trigger.) The check runs only while awake, so no
        // nifm IPC ever hits the sleep window. On an IP change we rebuild BOTH
        // sockets: a network teardown invalidates them, and the listener can
        // otherwise silently stop accepting (poll() doesn't always report it)
        // while mDNS keeps working, leaving the API unreachable on a live
        // console.
        if (++netcheck_ticks >= 20) { // ~2s at 100ms/iteration
            netcheck_ticks = 0;
            if (netif_ipv4_changed()) {
                LOGF("server: IP changed; rebuilding sockets\n");
                if (listen_fd >= 0) {
                    close(listen_fd);
                    listen_fd = -1;
                }
                if (mdns_fd >= 0) {
                    mdns_close(mdns_fd);
                    mdns_fd = -1;
                }
                    mdns_ready = false; // re-query the IP on the next iteration
            }
        }

        if (listen_fd < 0) {
            listen_fd = create_listener(cfg->port);
            if (listen_fd < 0) {
                LOGF("server: bind/listen on port %d failed (errno=%d), retrying\n",
                     cfg->port, errno);
                if (!retry_wait(idle))
                    return;
                continue;
            }
            LOGF("server: listening on port %d\n", cfg->port);
        }

        // (Re)establish mDNS advertising. The listener binds to INADDR_ANY and
        // succeeds even before DHCP finishes, so we can't gate this on the
        // listener: instead keep retrying here until the local IP is known and
        // the socket is open. Cheap no-op once mdns_fd is up.
        if (mdns_fd < 0) {
            if (!mdns_ready)
                mdns_ready = mdns_config_init(&mdns_cfg, cfg);
            if (mdns_ready) {
                mdns_fd = mdns_open(&mdns_cfg);
                if (mdns_fd >= 0) {
                    LOGF("server: mDNS up as %s\n", mdns_cfg.host);
                    mdns_announce_left = 3; // sent once routing is up (below)
                }
            }
        }

        // Send pending announcements, one per loop iteration, but only count
        // an announcement as sent when sendto() actually succeeds. Right after
        // a network change, routing isn't up yet and sends fail with
        // EHOSTUNREACH; retrying each iteration means we keep trying for as
        // long as it takes rather than burning the burst on dead sends.
        if (mdns_fd >= 0 && mdns_announce_left > 0) {
            if (mdns_announce(mdns_fd, &mdns_cfg))
                mdns_announce_left--;
        }

        struct pollfd pfds[2];
        pfds[0].fd = listen_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        nfds_t nfds = 1;
        int mdns_idx = -1;
        if (mdns_fd >= 0) {
            mdns_idx = (int)nfds;
            pfds[nfds].fd = mdns_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        int pr = poll(pfds, nfds, 100);

        if (idle && !idle())
            break;

        if (pr < 0) {
            // bsd service hiccup; rebuild both sockets.
            LOGF("server: poll failed (errno=%d), rebuilding listener\n", errno);
            close(listen_fd);
            listen_fd = -1;
            if (mdns_fd >= 0) {
                mdns_close(mdns_fd);
                mdns_fd = -1;
            }
            if (!retry_wait(idle))
                break;
            continue;
        }
        if (pr == 0)
            continue;

        // Service mDNS queries before HTTP accepts.
        if (mdns_idx >= 0 && (pfds[mdns_idx].revents & POLLIN))
            mdns_handle_readable(mdns_fd, &mdns_cfg);

        if (!(pfds[0].revents & POLLIN))
            continue;

        int client = accept(listen_fd, NULL, NULL);
        if (client < 0) {
            // EAGAIN: spurious wakeup on the non-blocking listener.
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                LOGF("server: accept failed (errno=%d)\n", errno);
            continue;
        }

        handle_connection(client, cfg);
        // Half-close the write side first so the peer reliably sees a FIN
        // (not an RST from leftover unread bytes), then close.
        shutdown(client, SHUT_WR);
        close(client);

        // Agent-requested power action: executed only after the response has
        // been sent and the connection closed, so the client gets its
        // confirmation before the console goes away.
        PowerAction act = power_take_scheduled();
        if (act != PowerAction_None) {
            LOGF("server: executing power action %d\n", act);
            svcSleepThread(200000000LL); // 200ms: let the response flush
            if (!power_perform(act))
                LOGF("server: power action failed\n");
            // For sleep, the PSC ReadySleep event arrives on a subsequent
            // iteration and quiesces sockets/HDLS as usual.
        }
    }

    http_set_idle_callback(NULL);
    if (listen_fd >= 0)
        close(listen_fd);
    if (mdns_fd >= 0)
        mdns_close(mdns_fd);
}
