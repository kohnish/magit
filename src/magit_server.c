#include <arpa/inet.h>
#include <git2.h>
#include <khash.h>
#include <msgpack.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <uv.h>
#include "msgpack_handler.h"
#include <git2/sys/errors.h>
#include "git_handler.h"

#define MAX_MESSAGE_SIZE (64 * 1024)
#define LENGTH_SIZE 4

enum parser_state {
    READ_LENGTH,
    READ_PAYLOAD
};

static enum parser_state g_state = READ_LENGTH;
static char g_read_buffer[65536];
static unsigned char g_length_buf[LENGTH_SIZE];
static size_t g_length_received = 0;
static char g_payload_buf[MAX_MESSAGE_SIZE];
static uint32_t g_payload_length = 0;
static size_t g_payload_received = 0;

static void protocol_error(uv_stream_t *stream, const char *msg) {
    fprintf(stderr, "protocol error: %s\n", msg);
    uv_close((uv_handle_t *)stream, NULL);
}

static void reset_parser(void) {
    g_state = READ_LENGTH;
    g_length_received = 0;
    g_payload_length = 0;
    g_payload_received = 0;
}

static void consume_bytes(uv_stream_t *stream, const unsigned char *data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        if (g_state == READ_LENGTH) {
            size_t need = LENGTH_SIZE - g_length_received;
            size_t available = len - offset;
            size_t copy = need < available ? need : available;
            memcpy(g_length_buf + g_length_received, data + offset, copy);
            g_length_received += copy;
            offset += copy;
            if (g_length_received < LENGTH_SIZE) {
                continue;
            }

            uint32_t network_length = 0;
            memcpy(&network_length, g_length_buf, sizeof(network_length));
            g_payload_length = ntohl(network_length);
            if (g_payload_length == 0) {
                protocol_error(stream, "zero length message");
                return;
            }
            if (g_payload_length > MAX_MESSAGE_SIZE) {
                protocol_error(stream, "message too large");
                return;
            }
            g_payload_received = 0;
            g_state = READ_PAYLOAD;
        }

        if (g_state == READ_PAYLOAD) {
            size_t need = g_payload_length - g_payload_received;
            size_t available = len - offset;
            size_t copy = need < available ? need : available;
            memcpy(g_payload_buf + g_payload_received, data + offset, copy);
            g_payload_received += copy;
            offset += copy;
            if (g_payload_received < g_payload_length) {
                continue;
            }
            msgpack_handler_recv(stream->loop, g_payload_buf, g_payload_length);
            reset_parser();
        }
    }
}

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    (void)handle;
    (void)suggested_size;
    *buf = uv_buf_init(g_read_buffer, sizeof(g_read_buffer));
}

static void read_stdin(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    if (nread < 0) {
        uv_close((uv_handle_t *)stream, NULL);
        return;
    }
    if (nread == 0) {
        return;
    }
    consume_bytes(stream, (unsigned char *)buf->base, (size_t)nread);
}

static void on_signal(uv_signal_t *handle, int signum) {
    if (signum == SIGINT) {
        uv_stop(handle->loop);
    }
}

int main(int argc, char* argv[]) {
    static uv_loop_t loop;
    static uv_tty_t tty_handle;
    static uv_signal_t sig_handle;

    git_libgit2_init();

    if (argc > 1) {
        if (git_handler_repo_init(argv[1]) != 0) {
            fprintf(stderr, "repo init fail");
            return -1;
        }
    }

    uv_loop_init(&loop);

    uv_tty_init(&loop, &tty_handle, STDIN_FILENO, 1);
    uv_read_start((uv_stream_t *)&tty_handle, alloc_buffer, read_stdin);

    uv_signal_init(&loop, &sig_handle);
    uv_signal_start(&sig_handle, on_signal, SIGINT);

    uv_run(&loop, UV_RUN_DEFAULT);

    if (argc > 1) {
        git_handler_repo_deinit();
    }
    git_libgit2_shutdown();

    return 0;
}
