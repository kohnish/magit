#include <git2.h>
#include <khash.h>
#include <kstring.h>
#include <msgpack.h>
#include <stdio.h>
#include <unity/unity.h>

void setUp(void) {}
void tearDown(void) {}

enum MAGIT_SERVER_MSG_KEY {
    MAGIT_SERVER_MSG_CMD_STR = 0
};

void make_message(msgpack_packer *packer, enum MAGIT_SERVER_MSG_KEY id, kstring_t *data) {
    msgpack_pack_int(packer, id);
    msgpack_pack_str(packer, data->l);
    msgpack_pack_str_body(packer, data->s, data->l);
}

void print_msgpack_bytes(const char *data, size_t size) {
    msgpack_unpacked result;
    msgpack_unpacked_init(&result);

    if (msgpack_unpack_next(&result, data, size, NULL)) {
        msgpack_object_print(stdout, result.data);
        printf("\n");
    } else {
        printf("invalid msgpack\n");
    }

    msgpack_unpacked_destroy(&result);
}

void read_msgpack_map(const char *bytes, size_t size) {
    msgpack_unpacked result;
    msgpack_unpacked_init(&result);

    if (msgpack_unpack_next(&result, bytes, size, NULL) != MSGPACK_UNPACK_SUCCESS) {
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

kstring_t kstr_create(const char *data) {
    kstring_t s = {0};
    kputs(data, &s);
    return s;
}

void kstr_cleanup(kstring_t *p) {
    free(p->s);
}

#define KSTR_CLEANUP __attribute__((cleanup(kstr_cleanup)))
#define MSGBUF_CLEANUP __attribute__((cleanup(msgpack_sbuffer_destroy)))

void git_branch(void) {
    git_repository *repo = NULL;
    git_reference *head = NULL;

    git_libgit2_init();

    if (git_repository_open(&repo, ".") != 0) {
        fprintf(stderr, "Failed to open repository\n");
        return;
    }

    if (git_repository_head(&head, repo) != 0) {
        fprintf(stderr, "No HEAD (possibly detached HEAD)\n");
        git_repository_free(repo);
        return;
    }

    const char *branch_name = git_reference_shorthand(head);

    if (branch_name) {
        printf("Current branch: %s\n", branch_name);
    }

    git_reference_free(head);
    git_repository_free(repo);
    git_libgit2_shutdown();
}

static void test_add_returns_sum(void) {
    int result = 0;
    MSGBUF_CLEANUP msgpack_sbuffer buf;
    msgpack_sbuffer_init(&buf);
    msgpack_packer packer;
    msgpack_packer_init(&packer, &buf, msgpack_sbuffer_write);
    msgpack_pack_map(&packer, 1);

    KSTR_CLEANUP kstring_t str = kstr_create("value");

    make_message(&packer, MAGIT_SERVER_MSG_CMD_STR, &str);
    read_msgpack_map(buf.data, buf.size);

    git_branch();
    TEST_ASSERT_EQUAL_INT(0, result);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_add_returns_sum);

    return UNITY_END();
}
