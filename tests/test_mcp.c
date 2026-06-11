// Host end-to-end tests for the MCP endpoint: real HTTP request parsing and
// mcp_handle_post over a socketpair, with input/screen stubbed (stubs.c).
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "base64.h"
#include "buttons.h"
#include "http.h"
#include "mcp.h"

extern uint64_t stub_tap_mask;
extern int stub_tap_duration;
extern int stub_tap_count;

// Issues one POST /mcp request with the given JSON body; returns the raw HTTP
// response in a static buffer.
static const char *do_rpc(const char *body) {
    static char resp[256 * 1024];
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    char req[64 * 1024];
    int rn = snprintf(req, sizeof(req),
                      "POST /mcp HTTP/1.1\r\nContent-Length: %zu\r\n\r\n%s",
                      strlen(body), body);
    assert(rn > 0 && write(sv[1], req, (size_t)rn) == rn);
    shutdown(sv[1], SHUT_WR);

    static HttpRequest hreq;
    assert(http_read_request(sv[0], &hreq));
    assert(strcmp(hreq.path, "/mcp") == 0);
    mcp_handle_post(&hreq);
    close(sv[0]);

    size_t total = 0;
    ssize_t n;
    while ((n = read(sv[1], resp + total, sizeof(resp) - 1 - total)) > 0)
        total += (size_t)n;
    resp[total] = '\0';
    close(sv[1]);
    return resp;
}

static void test_initialize(void) {
    const char *r = do_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                           "\"params\":{\"protocolVersion\":\"2025-03-26\","
                           "\"capabilities\":{},\"clientInfo\":{\"name\":\"t\",\"version\":\"0\"}}}");
    assert(strstr(r, "HTTP/1.1 200"));
    assert(strstr(r, "\"id\":1"));
    assert(strstr(r, "\"protocolVersion\":\"2025-03-26\"")); // echoed known version
    assert(strstr(r, "\"serverInfo\":{\"name\":\"sys-autopilot\""));
    assert(strstr(r, "\"tools\":{}"));
    printf("initialize ok\n");
}

static void test_notification(void) {
    const char *r = do_rpc("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    assert(strstr(r, "HTTP/1.1 202"));
    printf("notification ok\n");
}

static void test_ping_and_errors(void) {
    const char *r = do_rpc("{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":\"ping\"}");
    assert(strstr(r, "\"id\":\"abc\""));
    assert(strstr(r, "\"result\":{}"));

    r = do_rpc("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"bogus/method\"}");
    assert(strstr(r, "\"error\":{\"code\":-32601"));

    r = do_rpc("this is not json");
    assert(strstr(r, "-32700"));

    r = do_rpc("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
               "\"params\":{\"name\":\"no_such_tool\"}}");
    assert(strstr(r, "\"error\":{\"code\":-32602"));
    printf("ping/errors ok\n");
}

static void test_tools_list(void) {
    const char *r = do_rpc("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/list\"}");
    assert(strstr(r, "\"tap_buttons\""));
    assert(strstr(r, "\"upload_file\""));
    assert(strstr(r, "\"screenshot\""));
    assert(strstr(r, "\"inputSchema\""));
    printf("tools/list ok\n");
}

static void test_tap_buttons(void) {
    stub_tap_count = 0;
    const char *r = do_rpc("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
                           "\"params\":{\"name\":\"tap_buttons\",\"arguments\":"
                           "{\"buttons\":[\"A\",\"home\"],\"durationMs\":5}}}");
    assert(strstr(r, "\"isError\":false"));
    assert(stub_tap_count == 1);
    assert(stub_tap_mask == (BTN_A | BTN_HOME));
    assert(stub_tap_duration == 5);

    // Unknown button -> in-band tool error.
    r = do_rpc("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\","
               "\"params\":{\"name\":\"tap_buttons\",\"arguments\":{\"buttons\":[\"Q\"]}}}");
    assert(strstr(r, "\"isError\":true"));
    printf("tap_buttons ok\n");
}

static void test_tap_sequence(void) {
    stub_tap_count = 0;
    const char *r = do_rpc("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
                           "\"params\":{\"name\":\"tap_sequence\",\"arguments\":{\"taps\":["
                           "{\"buttons\":[\"RIGHT\"],\"delayAfterMs\":1},"
                           "{\"buttons\":[\"RIGHT\"],\"delayAfterMs\":1},"
                           "{\"buttons\":[\"A\"],\"durationMs\":2}]}}}");
    assert(strstr(r, "performed 3 taps"));
    assert(stub_tap_count == 3);
    assert(stub_tap_mask == BTN_A);
    printf("tap_sequence ok\n");
}

static void test_screenshot(void) {
    const char *r = do_rpc("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\","
                           "\"params\":{\"name\":\"screenshot\",\"arguments\":{}}}");
    // base64("FAKEJPEGDATA")
    char expect[64];
    size_t n = b64_encode((const uint8_t *)"FAKEJPEGDATA", 12, expect);
    expect[n] = '\0';
    assert(strstr(r, "\"type\":\"image\""));
    assert(strstr(r, expect));
    assert(strstr(r, "\"mimeType\":\"image/jpeg\""));
    printf("screenshot ok\n");
}

static void test_upload_and_files(void) {
    system("rm -rf " FAKE_SD " && mkdir -p " FAKE_SD);

    // Upload with content BEFORE path (key-order robustness), then read back.
    const char *payload = "Hello, upload!\nLine two.";
    char b64[128];
    size_t n = b64_encode((const uint8_t *)payload, strlen(payload), b64);
    b64[n] = '\0';

    char body[512];
    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"upload_file\",\"arguments\":"
             "{\"content\":\"%s\",\"path\":\"/up/test.txt\"}}}", b64);
    const char *r = do_rpc(body);
    assert(strstr(r, "\"isError\":false"));
    assert(strstr(r, "wrote 24 bytes"));

    FILE *f = fopen(FAKE_SD "/up/test.txt", "rb");
    assert(f);
    char readback[64] = {0};
    size_t got = fread(readback, 1, sizeof(readback), f);
    fclose(f);
    assert(got == strlen(payload) && memcmp(readback, payload, got) == 0);

    // read_file round-trip (content contains a quote + newline after this write).
    r = do_rpc("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\","
               "\"params\":{\"name\":\"read_file\",\"arguments\":{\"path\":\"/up/test.txt\"}}}");
    assert(strstr(r, "Hello, upload!\\nLine two."));

    // read_file with negative offset (tail).
    r = do_rpc("{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
               "\"params\":{\"name\":\"read_file\",\"arguments\":"
               "{\"path\":\"/up/test.txt\",\"offset\":-9}}}");
    assert(strstr(r, "Line two."));
    assert(!strstr(r, "Hello"));

    // list_directory.
    r = do_rpc("{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\","
               "\"params\":{\"name\":\"list_directory\",\"arguments\":{\"path\":\"/up\"}}}");
    assert(strstr(r, "test.txt"));
    assert(strstr(r, "\\\"type\\\":\\\"file\\\"")); // listing JSON is escaped text

    // delete_file + temp cleanup check.
    r = do_rpc("{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/call\","
               "\"params\":{\"name\":\"delete_file\",\"arguments\":{\"path\":\"/up/test.txt\"}}}");
    assert(strstr(r, "deleted"));
    struct stat st;
    assert(stat(FAKE_SD "/up/test.txt", &st) != 0);
    assert(stat(MCP_UPLOAD_TMP, &st) != 0); // no leftover temp file

    // Invalid base64 content -> tool error, no file created.
    r = do_rpc("{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"tools/call\","
               "\"params\":{\"name\":\"upload_file\",\"arguments\":"
               "{\"content\":\"!!notbase64!!\",\"path\":\"/up/bad.bin\"}}}");
    assert(strstr(r, "\"isError\":true"));
    assert(stat(FAKE_SD "/up/bad.bin", &st) != 0);
    assert(stat(MCP_UPLOAD_TMP, &st) != 0);

    // Path traversal rejected.
    r = do_rpc("{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"tools/call\","
               "\"params\":{\"name\":\"read_file\",\"arguments\":{\"path\":\"/../etc/passwd\"}}}");
    assert(strstr(r, "\"isError\":true"));
    printf("upload/files ok\n");
}

int main(void) {
    test_initialize();
    test_notification();
    test_ping_and_errors();
    test_tools_list();
    test_tap_buttons();
    test_tap_sequence();
    test_screenshot();
    test_upload_and_files();
    printf("all mcp tests passed\n");
    return 0;
}
