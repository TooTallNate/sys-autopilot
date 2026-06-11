// Host-side unit test for source/common/http.c (pure POSIX code).
#include "http.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>

static int feed(const char *data) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(write(sv[1], data, strlen(data)) == (ssize_t)strlen(data));
    shutdown(sv[1], SHUT_WR);
    return sv[0];
}

static HttpRequest req;

int main(void) {
    // 1. Basic GET with query params + URL decoding
    int fd = feed("GET /files?path=%2Fswitch%2Fmy%20app.log&offset=-100 HTTP/1.1\r\nHost: x\r\n\r\n");
    assert(http_read_request(fd, &req));
    assert(strcmp(req.method, "GET") == 0);
    assert(strcmp(req.path, "/files") == 0);
    char val[256];
    assert(http_query_get(&req, "path", val, sizeof(val)));
    assert(strcmp(val, "/switch/my app.log") == 0);
    assert(http_query_get(&req, "offset", val, sizeof(val)));
    assert(strcmp(val, "-100") == 0);
    assert(!http_query_get(&req, "length", val, sizeof(val)));
    close(fd);

    // 2. POST with buttons CSV
    fd = feed("POST /input/tap?buttons=A,B,HOME&durationMs=250 HTTP/1.1\r\n\r\n");
    assert(http_read_request(fd, &req));
    assert(strcmp(req.method, "POST") == 0);
    assert(http_query_get(&req, "buttons", val, sizeof(val)));
    assert(strcmp(val, "A,B,HOME") == 0);
    close(fd);

    // 3. PUT with body split across header read
    fd = feed("PUT /files?path=%2Ftest.txt HTTP/1.1\r\nContent-Length: 11\r\n\r\nhello world");
    assert(http_read_request(fd, &req));
    assert(req.has_content_length && req.content_length == 11);
    char body[64] = {0};
    size_t total = 0;
    ssize_t n;
    while ((n = http_read_body(&req, body + total, sizeof(body) - total)) > 0)
        total += (size_t)n;
    assert(total == 11);
    assert(memcmp(body, "hello world", 11) == 0);
    close(fd);

    // 4. Basic auth: valid credentials (user:pass = dXNlcjpwYXNz)
    fd = feed("GET /status HTTP/1.1\r\nAuthorization: Basic dXNlcjpwYXNz\r\n\r\n");
    assert(http_read_request(fd, &req));
    assert(http_check_basic_auth(&req, "user", "pass"));
    assert(!http_check_basic_auth(&req, "user", "wrong"));
    assert(!http_check_basic_auth(&req, "user", "pas"));
    close(fd);

    // 5. No auth header
    fd = feed("GET /status HTTP/1.1\r\n\r\n");
    assert(http_read_request(fd, &req));
    assert(!http_check_basic_auth(&req, "user", "pass"));
    assert(!http_check_bearer_auth(&req, "sekrit"));
    close(fd);

    // 5b. Bearer token auth
    fd = feed("GET /status HTTP/1.1\r\nAuthorization: Bearer sekrit\r\n\r\n");
    assert(http_read_request(fd, &req));
    assert(http_check_bearer_auth(&req, "sekrit"));
    assert(!http_check_bearer_auth(&req, "sekri"));
    assert(!http_check_bearer_auth(&req, "sekrit2"));
    assert(!http_check_basic_auth(&req, "user", "pass"));
    close(fd);

    // 6. Malformed request
    fd = feed("GARBAGE\r\n\r\n");
    assert(!http_read_request(fd, &req));
    close(fd);

    // 7. Empty-value query param
    fd = feed("GET /x?a=&b=2 HTTP/1.1\r\n\r\n");
    assert(http_read_request(fd, &req));
    assert(http_query_get(&req, "a", val, sizeof(val)) && val[0] == '\0');
    assert(http_query_get(&req, "b", val, sizeof(val)) && strcmp(val, "2") == 0);
    close(fd);


    // 8. Non-blocking socket with delayed body delivery (exercises the
    //    EAGAIN -> poll() wait path in io_recv).
    {
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        // Make the server-side fd non-blocking, like server.c does.
        int flags = fcntl(sv[0], F_GETFL, 0);
        assert(fcntl(sv[0], F_SETFL, flags | O_NONBLOCK) == 0);

        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            // Child: send headers, pause, then send the body in two chunks.
            close(sv[0]);
            const char *hdrs = "PUT /files?path=%2Fa.txt HTTP/1.1\r\nContent-Length: 10\r\n\r\n";
            assert(write(sv[1], hdrs, strlen(hdrs)) > 0);
            usleep(150 * 1000);
            assert(write(sv[1], "01234", 5) == 5);
            usleep(150 * 1000);
            assert(write(sv[1], "56789", 5) == 5);
            close(sv[1]);
            _exit(0);
        }
        close(sv[1]);
        assert(http_read_request(sv[0], &req));
        assert(req.content_length == 10);
        char body[32] = {0};
        size_t total = 0;
        ssize_t n;
        while ((n = http_read_body(&req, body + total, sizeof(body) - total)) > 0)
            total += (size_t)n;
        assert(total == 10);
        assert(memcmp(body, "0123456789", 10) == 0);
        close(sv[0]);
        int st; waitpid(pid, &st, 0);
    }

    printf("all http tests passed\n");
    return 0;
}
