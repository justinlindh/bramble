/*
 * Per-device RPC auth token provider, shared by every transport (WebSocket on
 * ESP, BLE on all fleets, serial). Extracted from ws_server.c so transports
 * that build without the HTTP server (the nRF target) get the same fail-closed
 * token semantics from one implementation. The ws_server_* names predate the
 * extraction and are kept: three transports and the webapp docs use them.
 */
#include "ws_server.h"

#include <string.h>

#include "esp_log.h"
#include "identity.h"

#include "ct_strcmp.h"

static const char* TAG = "rpc_token";

static char s_auth_token[WS_AUTH_TOKEN_MAX] = {0};
/* True when the token could not be provided or persisted. Fail CLOSED: full
 * RPC access is impossible until the condition clears; only the
 * unauthenticated pairing allowlist is served. */
static bool s_token_unavailable = false;
/* True when the ONLY thing blocking a first-boot mint was the SEC-L1 entropy
 * gate. That is a transient, recoverable condition (the gate opens at RF
 * bring-up), unlike an NVS fault, so the mint is retried lazily on the next
 * auth evaluation instead of leaving the node permanently unreachable. */
static bool s_token_pending_entropy = false;

void ws_server_load_token(void) {
    int rc = identity_ensure_ws_auth_token(s_auth_token, sizeof(s_auth_token));
    if (rc == IDENTITY_TOKEN_ERR_ENTROPY) {
        /* First boot, entropy gate still shut. Nothing was minted and nothing
         * was persisted, so this is retryable: stay fail CLOSED for now and
         * mint on the next auth evaluation (auth_eval), by which point an RF
         * subsystem is necessarily up and the gate is open. Loud on purpose:
         * a node that stays in this state is one nobody can pair with. */
        s_auth_token[0] = '\0';
        s_token_unavailable = true;
        s_token_pending_entropy = true;
        ESP_LOGW(TAG, "Auth token not minted yet (entropy gate shut); RPC limited to pairing "
                      "allowlist until entropy is ready");
        return;
    }
    if (rc < 0) {
        /* NVS could not provide or persist a token. Fail CLOSED: no
         * credentials can match, so only the pairing allowlist is
         * reachable until the token store recovers. Not retried: an NVS
         * fault does not clear on its own. */
        s_auth_token[0] = '\0';
        s_token_unavailable = true;
        s_token_pending_entropy = false;
        ESP_LOGE(TAG, "Auth token unavailable (NVS error); RPC limited to pairing allowlist");
        return;
    }
    s_token_unavailable = false;
    s_token_pending_entropy = false;
    if (s_auth_token[0] != '\0') {
        ESP_LOGI(TAG, "RPC auth enabled (per-device token)");
    } else {
        ESP_LOGW(TAG, "RPC auth disabled by explicit opt-out (open access)");
    }
}

const char* ws_server_get_token(void) { return s_auth_token; }

bool ws_server_auth_disabled(void) { return !s_token_unavailable && s_auth_token[0] == '\0'; }

bool ws_token_pending_entropy(void) { return s_token_pending_entropy; }

bool ws_token_matches(const char* candidate) {
    return !s_token_unavailable && ct_strcmp(candidate, s_auth_token) == 0;
}
