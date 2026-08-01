#include "codes.h"
#include "kstring.h"
#include "util.h"
#include <msgpack.h>
#include <msgpack/object.h>
#include <msgpack/sbuffer.h>
#include <stdint.h>
#include <uv.h>
#include "msgpack_handler.h"
#include "git_handler.h"

#define MSGBUF_CLEANUP __attribute__((cleanup(msgpack_sbuffer_destroy)))
#define MSG_UNPACKED_CLEANUP __attribute__((cleanup(msgpack_unpacked_destroy)))


static void append_key_val_str(msgpack_packer *packer, enum MAGIT_RES_KEY key, kstring_t *val) {
    msgpack_pack_int(packer, key);
    msgpack_pack_str(packer, val->l);
    msgpack_pack_str_body(packer, val->s, val->l);
}

static void append_key_val_int(msgpack_packer *packer, enum MAGIT_RES_KEY key, uint64_t val) {
    msgpack_pack_int(packer, key);
    msgpack_pack_int(packer, val);
}

msgpack_sbuffer msgpack_handler_buf_create(magit_res_T *res) {
    msgpack_sbuffer buf;
    msgpack_sbuffer_init(&buf);
    msgpack_packer packer;
    msgpack_packer_init(&packer, &buf, msgpack_sbuffer_write);
    msgpack_pack_map(&packer, 2);

    append_key_val_int(&packer, MAGIT_RES_KEY_ID, res->id);
    append_key_val_str(&packer, MAGIT_RES_KEY_GIT_ROOT, res->git_root);
    return buf;
}


static void print_msgpack_bytes(const char *data, size_t size) {
    msgpack_unpacked result;
    msgpack_unpacked_init(&result);

    if (msgpack_unpack_next(&result, data, size, NULL) != MSGPACK_UNPACK_SUCCESS) {
        printf("Failed to unpack\n");
        msgpack_unpacked_destroy(&result);
        return;
    }

    msgpack_object obj = result.data;

    if (obj.type != MSGPACK_OBJECT_MAP) {
        printf("Expected a map\n");
        msgpack_unpacked_destroy(&result);
        return;
    }

    for (uint32_t i = 0; i < obj.via.map.size; i++) {
        msgpack_object key = obj.via.map.ptr[i].key;
        msgpack_object val = obj.via.map.ptr[i].val;

        if (key.type == MSGPACK_OBJECT_POSITIVE_INTEGER) {
            printf("key: %llu, ", (unsigned long long)key.via.u64);
        }

        switch (val.type) {
        case MSGPACK_OBJECT_POSITIVE_INTEGER:
            printf("value: %llu\n", (unsigned long long)val.via.u64);
            break;

        case MSGPACK_OBJECT_STR:
            printf("value: %.*s\n", val.via.str.size, val.via.str.ptr);
            break;

        default:
            printf("unsupported value type\n");
            break;
        }
    }
    msgpack_unpacked_destroy(&result);
}

int msgpack_handler_send(magit_res_T *res) {
    MSGBUF_CLEANUP msgpack_sbuffer msg = msgpack_handler_buf_create(res);

    uint32_t len = (uint32_t)msg.size;

    uint8_t header[4] = {
        (uint8_t)((len >> 24) & 0xff),
        (uint8_t)((len >> 16) & 0xff),
        (uint8_t)((len >> 8) & 0xff),
        (uint8_t)(len & 0xff)
    };

    fwrite(header, 1, sizeof(header), stdout);
    fwrite(msg.data, 1, msg.size, stdout);

    // print_msgpack_bytes(msg.data, msg.size);

    fflush(stdout);

    return 0;
}

int msgpack_handler_recv(uv_loop_t *loop, const char *data, size_t len) {

    MSG_UNPACKED_CLEANUP msgpack_unpacked result;
    msgpack_unpacked_init(&result);

    if (msgpack_unpack_next(&result, data, len, NULL) != MSGPACK_UNPACK_SUCCESS) {
        return -1;
    }

    msgpack_object *obj = &result.data;
    if (obj->type != MSGPACK_OBJECT_MAP) {
        return -1;
    }

    uint64_t id = 0;
    uint64_t cmd = 0;
    STR_CLEANUP kstring_t *pwd = NULL;

    for (uint32_t i = 0; i < obj->via.map.size; i++) {
        msgpack_object key = obj->via.map.ptr[i].key;
        msgpack_object val = obj->via.map.ptr[i].val;

        if (key.type != MSGPACK_OBJECT_POSITIVE_INTEGER) {
            return ERR_INVALID_KEY_TYPE;
        }

        switch ((enum MSGKEY)key.via.u64) {
        case MSGKEY_ID_UINT:
            if (val.type != MSGPACK_OBJECT_POSITIVE_INTEGER) {
                return ERR_INVALID_KEY_VAL_TYPE;
            }
            id = val.via.u64;
            break;
        case MSGKEY_CMD_UINT:
            if (val.type != MSGPACK_OBJECT_POSITIVE_INTEGER) {
                return ERR_INVALID_CMD_VAL_TYPE;
            }
            cmd = val.via.u64;
            break;
        case MSGKEY_PWD_STR:
            if (val.type != MSGPACK_OBJECT_STR) {
                return ERR_INVALID_PWD_VAL_TYPE;
            }
            pwd = str_create(val.via.str.ptr, val.via.str.size);
            break;
        };
    }

    if (id != 0 && cmd != 0) {
        switch ((enum CMD)cmd) {
            case CMD_GIT_STATUS:
                git_handler_queue_git_status(loop, id, xfer(pwd));
                break;
            }
    }
    return ERR_UNEXPECTED_MSG;
}
