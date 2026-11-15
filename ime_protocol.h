/*
 * ime_protocol.h — conhost <-> leopard-ime manager wire protocol.
 *
 * Keep byte-for-byte in sync with leopard-ime/protocol.h.  Frames are
 * length-prefixed, little-endian, and carry no key contents in any log path.
 */
#pragma once

#include <stdint.h>

#define IME_PROTO_MAGIC   0x314d494cu /* "LIM1" */
#define IME_PROTO_VERSION 1u
#define IME_FRAME_HEADER_SIZE 12u
#define IME_MAX_FRAME     4096u

typedef struct ime_frame_header {
        uint32_t magic;
        uint16_t version;
        uint16_t type;
        uint32_t length; /* payload bytes following this header */
} ime_frame_header_t;

enum ime_message_type {
        IME_MSG_HELLO   = 1, /* client -> manager */
        IME_MSG_WELCOME = 2, /* manager -> client */
        IME_MSG_FOCUS   = 3, /* client -> manager */
        IME_MSG_KEY     = 4, /* client -> manager */
        IME_MSG_REPLY   = 5, /* manager -> client */
        IME_MSG_UI      = 6, /* manager -> client */
        IME_MSG_BYE     = 7,
        IME_MSG_STATE   = 8, /* manager -> client: active/composing state */
};

enum ime_reply_action {
        IME_ACTION_PASS        = 0, /* write the original key bytes */
        IME_ACTION_COMMIT      = 1, /* write commit bytes only */
        IME_ACTION_CONSUMED    = 2, /* write nothing */
        IME_ACTION_COMMIT_PASS = 3, /* write commit bytes, then the original */
};

enum ime_key_kind {
        IME_KEY_CHAR    = 1,
        IME_KEY_KEYDOWN = 2,
        IME_KEY_KEYUP   = 3,
};

#define IME_KEY_FLAG_TOGGLE 0x1u /* Ctrl+Space observed by conhost */

#define IME_CTX_TERMINAL 0x1u
#define IME_CTX_PASSWORD 0x2u /* no-echo: IME must bypass */

#define IME_CAP_TERMINAL_UI 0x1u

typedef struct ime_hello {
        uint32_t pid;
        uint32_t abi;
        uint32_t max_frame;
} ime_hello_t;

typedef struct ime_welcome {
        uint32_t session;
        uint32_t capabilities;
        uint32_t max_commit;
} ime_welcome_t;

typedef struct ime_focus {
        uint32_t flags; /* IME_CTX_* */
} ime_focus_t;

#define IME_STATE_ACTIVE    0x1u
#define IME_STATE_COMPOSING 0x2u
typedef struct ime_state {
        uint32_t flags; /* IME_STATE_* */
        uint32_t mode;  /* reserved: 0 direct, 1 compose */
} ime_state_t;

typedef struct ime_key {
        uint32_t seq;
        uint32_t kind;  /* IME_KEY_* */
        uint32_t value; /* CHAR: packed UTF-8; KEY: key code */
        uint32_t flags; /* IME_KEY_FLAG_* */
        uint32_t length; /* original terminal bytes that follow */
} ime_key_t;

typedef struct ime_reply {
        uint32_t seq;
        uint32_t action; /* IME_ACTION_* */
        uint32_t length; /* commit bytes (for COMMIT / COMMIT_PASS) */
} ime_reply_t;

typedef struct ime_ui {
        uint32_t flags;       /* bit 0 preedit, bit 1 candidates */
        uint32_t cursor;      /* preedit cursor byte offset */
        uint32_t preedit_len; /* bytes following the header */
        uint32_t cand_count;  /* u16-length-prefixed candidates follow */
} ime_ui_t;

#define IME_UI_HAS_PREEDIT    0x1u
#define IME_UI_HAS_CANDIDATES 0x2u

#define IME_MAX_COMMIT 256u
#define IME_MAX_PREEDIT 128u
#define IME_MAX_CANDIDATES 9u
#define IME_MAX_CANDIDATE_BYTES 64u

