// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "magpie_model.h"

/*
 * Decoder for the device-pull display protocol (v1).
 *
 * Pure C, no ESP-IDF and no I/O: the BLE central and the HTTP fallback both
 * hand their received bytes to these functions, and the host tests decode the
 * exact payloads the companion produces (tests/host/fixtures/payloads).
 *
 * Payload shapes (see docs/protocol.md):
 *
 *   INDEX  {"v":1,"t":<epoch>,"c":<count>,"lo":{"i","n","w","u","r"},
 *           "ids":[0,1,..]}
 *   SUB    {"i":<slot>,"p":<provider>,"n":<name>,"pl":<plan>,"e":<0|1>,
 *           "w":[{"n":"5h","u":84,"r":<epoch>}...]}
 */

#define MAGPIE_OK 0
#define MAGPIE_ERR_INVALID_ARG 0x102
#define MAGPIE_ERR_INVALID_RESPONSE 0x10c

/** Decode an INDEX payload into out. */
int magpie_decode_index(const char *body, size_t len, magpie_index_t *out);

/**
 * Decode a SUB payload into out.
 *
 * The payload's own "i" identifies the slot; @p expected_slot must match it
 * (use -1 to accept any). A mismatch is an invalid response, so stale reads
 * cannot overwrite another page.
 */
int magpie_decode_sub(const char *body, size_t len, int expected_slot,
                      magpie_sub_t *out);

/** Reset an index to its empty state. */
void magpie_index_clear(magpie_index_t *index);

/** Reset a subscription to its empty state (keeps the slot id). */
void magpie_sub_clear(magpie_sub_t *sub, uint8_t slot);

/** Tiny helpers shared with the transport layer. */
bool magpie_payload_fits(size_t len, size_t limit);
