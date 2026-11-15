/*
 * ime_client.h — fail-open conhost client for the terminal IME manager.
 *
 * Keys are forwarded to the manager and withheld only until a reply (or a
 * short deadline) arrives.  On any transport failure, timeout, or malformed
 * reply the original bytes are replayed in order: the IME can never swallow a
 * keystroke.  The client also carries focus/no-echo context and the manager's
 * preedit/candidate state for the terminal status overlay.
 */
#pragma once

#include <stdint.h>
#include <sys/types.h>

#include "ime_protocol.h"

#define IME_CLIENT_QUEUE 64
#define IME_REPLY_TIMEOUT_MS 100u
#define IME_OVERLAY_MAX 192u

typedef struct ime_pending {
        uint32_t seq;
        uint64_t deadline_ms;
        uint8_t  used;
        uint8_t  ready;
        uint8_t  action;
        uint8_t  length;
        uint8_t  original[32];
        uint8_t  output[IME_MAX_COMMIT];
        uint16_t output_len;
} ime_pending_t;

typedef struct ime_client {
        int      in_fd;
        int      out_fd;
        int      pid;
        int      enabled;
        int      active;     /* Ctrl+Space on/off, cached locally */
        int      composing;  /* manager UI says a compose run is active */
        int      welcomed;
        int      failed;
        uint32_t next_seq;
        uint32_t head_seq;
        uint32_t session;
        uint32_t context;
        uint32_t sent_context;
        uint32_t consecutive_timeouts;

        uint8_t  rx[IME_MAX_FRAME];
        size_t   rx_len;

        ime_pending_t pending[IME_CLIENT_QUEUE];

        char     overlay[IME_OVERLAY_MAX];
        size_t   overlay_len;
} ime_client_t;

typedef void (*ime_emit_fn)(void *ctx, const void *bytes, size_t len);

uint64_t ime_now_ms(void);

void ime_client_init(ime_client_t *client);

/* Create the stdin/stdout pipe pair and fork the manager.  Returns 0 when the
 * manager is running, -1 when the caller must run without IME. */
int  ime_client_start(ime_client_t *client, const char *manager_path);

int  ime_client_fd(const ime_client_t *client);
int  ime_client_connected(const ime_client_t *client);

/* Update focus/context.  Re-sent only when it changes. */
void ime_client_set_context(ime_client_t *client, uint32_t flags);

/* Queue one key.  Returns 1 when the manager owns it (do not emit directly),
 * 0 when the caller must emit the bytes itself. */
int  ime_client_key(ime_client_t *client, uint32_t kind, uint32_t value,
                    uint32_t flags, const void *bytes, size_t len);

/* Drain replies, emit resolved keys through emit(), and enforce deadlines. */
void ime_client_pump(ime_client_t *client, uint64_t now_ms,
                     ime_emit_fn emit, void *ctx);

/* Terminal overlay text (preedit + candidates), or NULL when idle. */
const char *ime_client_overlay(ime_client_t *client, size_t *len);

void ime_client_close(ime_client_t *client);
