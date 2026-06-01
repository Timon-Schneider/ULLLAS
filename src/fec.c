#include "fec.h"
#include <stdlib.h>
#include <string.h>

/* ---------- sender ---------- */

int fec_tx_init(FecTxState *st, int group_size, size_t payload_size) {
    memset(st, 0, sizeof(*st));
    if (group_size < 2 || group_size > FEC_MAX_GROUP) return -1;
    if (payload_size == 0) return -1;
    st->xor_buf = (uint8_t *)calloc(1, payload_size);
    if (!st->xor_buf) return -1;
    st->enabled            = 1;
    st->group_size         = group_size;
    st->payload_size       = payload_size;
    st->received_in_group  = 0;
    return 0;
}

void fec_tx_destroy(FecTxState *st) {
    if (!st) return;
    free(st->xor_buf);
    st->xor_buf = NULL;
    st->enabled = 0;
}

int fec_tx_on_data(FecTxState *st, const uint8_t *payload, size_t payload_len) {
    if (!st->enabled) return 0;
    if (payload_len > st->payload_size) payload_len = st->payload_size;

    /* XOR payload into accumulator. Trailing bytes (if payload is short)
     * left as their previous values - the XOR identity for "no
     * contribution" is zero. We rely on the size constraint enforced at
     * sender init so this normally never happens. */
    uint8_t       *acc = st->xor_buf;
    const uint8_t *src = payload;
    for (size_t i = 0; i < payload_len; i++) acc[i] ^= src[i];

    st->received_in_group++;
    if (st->received_in_group >= st->group_size) {
        st->received_in_group = 0;
        return 1; /* emit parity */
    }
    return 0;
}

const uint8_t *fec_tx_parity_buf(const FecTxState *st) {
    return st->xor_buf;
}

/* The accumulator must be zeroed before the next group. We reset it
 * eagerly inside fec_tx_on_data after the caller has consumed the
 * parity payload via fec_tx_parity_buf. The caller signals consumption
 * by calling this helper. */
static void fec_tx_reset_after_emit(FecTxState *st) {
    memset(st->xor_buf, 0, st->payload_size);
}

/* ---------- receiver ---------- */

int fec_rx_init(FecRxState *st, int group_size, size_t payload_size) {
    memset(st, 0, sizeof(*st));
    if (group_size < 2 || group_size > FEC_MAX_GROUP) return -1;
    if (payload_size == 0) return -1;

    st->group_size  = group_size;
    st->window_size = group_size + 1;
    st->payload_size = payload_size;
    st->payloads     = (uint8_t *)calloc((size_t)st->window_size, payload_size);
    st->slot_kind    = (uint8_t *)calloc((size_t)st->window_size, 1);
    st->slot_samples = (uint16_t *)calloc((size_t)st->window_size, sizeof(uint16_t));
    if (!st->payloads || !st->slot_kind || !st->slot_samples) {
        fec_rx_destroy(st);
        return -1;
    }
    st->enabled  = 1;
    st->head_seq = 0;
    st->head_idx = 0;
    st->started  = 0;
    return 0;
}

void fec_rx_destroy(FecRxState *st) {
    if (!st) return;
    free(st->payloads);
    free(st->slot_kind);
    free(st->slot_samples);
    st->payloads     = NULL;
    st->slot_kind    = NULL;
    st->slot_samples = NULL;
    st->enabled      = 0;
}

void fec_rx_reset(FecRxState *st) {
    if (!st->enabled) return;
    memset(st->slot_kind, 0, (size_t)st->window_size);
    st->head_seq = 0;
    st->head_idx = 0;
    st->started  = 0;
}

static inline int slot_index(const FecRxState *st, int offset_from_head) {
    int idx = st->head_idx + offset_from_head;
    while (idx >= st->window_size) idx -= st->window_size;
    return idx;
}

/* Flush slot at head_idx if data; advance head. */
static void emit_head_slot(FecRxState *st, FecRxEmitCb emit, void *ud) {
    int       idx  = st->head_idx;
    uint8_t   kind = st->slot_kind[idx];
    uint32_t  seq  = st->head_seq;
    uint16_t  ns   = st->slot_samples[idx];
    uint8_t  *pl   = st->payloads + (size_t)idx * st->payload_size;

    if (kind == 1) {
        emit(seq, ns, pl, 0, ud);
    } else if (kind == 0) {
        st->unrecoverable_count++;
        emit(seq, st->expected_num_samples, NULL, 0, ud);
    }
    /* parity slot: just dropped, nothing to emit */

    st->slot_kind[idx] = 0;
    st->head_idx       = (st->head_idx + 1) % st->window_size;
    st->head_seq++;
}

static void try_recover_group(FecRxState *st) {
    /* The window holds N+1 slots: positions 0..N-1 are data, position N
     * is parity. If the parity slot is populated and exactly one data
     * slot is empty, recover it via XOR of all received data + parity. */
    int parity_idx_off = st->group_size;
    int parity_slot    = slot_index(st, parity_idx_off);
    if (st->slot_kind[parity_slot] != 2) return;

    int missing_off = -1;
    int missing_count = 0;
    for (int i = 0; i < st->group_size; i++) {
        int s = slot_index(st, i);
        if (st->slot_kind[s] == 0) {
            missing_off = i;
            missing_count++;
        }
    }
    if (missing_count != 1) return;

    /* XOR all received data payloads with the parity payload into the
     * missing slot's storage. */
    int      target_idx  = slot_index(st, missing_off);
    uint8_t *target      = st->payloads + (size_t)target_idx * st->payload_size;
    uint8_t *parity      = st->payloads + (size_t)parity_slot * st->payload_size;
    memcpy(target, parity, st->payload_size);
    for (int i = 0; i < st->group_size; i++) {
        if (i == missing_off) continue;
        int            s   = slot_index(st, i);
        const uint8_t *src = st->payloads + (size_t)s * st->payload_size;
        for (size_t b = 0; b < st->payload_size; b++) target[b] ^= src[b];
    }
    st->slot_kind[target_idx]    = 1;
    st->slot_samples[target_idx] = st->expected_num_samples;
    st->recovered_count++;
}

void fec_rx_process(FecRxState *st, uint32_t seq, int is_parity, uint16_t num_samples, const uint8_t *payload,
                    FecRxEmitCb emit, void *ud) {
    if (!st->enabled) return;

    if (num_samples > 0 && st->expected_num_samples == 0) {
        st->expected_num_samples = num_samples;
    }

    if (!st->started) {
        /* Establish head_seq at the start of the group containing this
         * packet so window alignment is well-defined. */
        uint32_t group_first = seq - (seq % (uint32_t)st->window_size);
        st->head_seq         = group_first;
        st->head_idx         = 0;
        memset(st->slot_kind, 0, (size_t)st->window_size);
        st->started = 1;
    }

    /* Sender restart / large backwards jump: drop everything and re-arm. */
    if (seq + (uint32_t)(st->window_size * 4) < st->head_seq) {
        fec_rx_flush(st, emit, ud);
        fec_rx_reset(st);
        uint32_t group_first = seq - (seq % (uint32_t)st->window_size);
        st->head_seq         = group_first;
        st->head_idx         = 0;
        st->started          = 1;
    }

    /* If incoming seq is older than head, it's a late duplicate - drop. */
    if (seq < st->head_seq) return;

    /* Advance window until the seq fits. Each advance may finalize an
     * older group (attempt recovery, then emit). */
    while (seq >= st->head_seq + (uint32_t)st->window_size) {
        try_recover_group(st);
        for (int i = 0; i < st->window_size; i++) emit_head_slot(st, emit, ud);
    }

    int offset = (int)(seq - st->head_seq);
    int idx    = slot_index(st, offset);
    if (st->slot_kind[idx] != 0) {
        /* Duplicate (same seq seen twice). Ignore the second copy. */
        return;
    }
    if (payload) {
        memcpy(st->payloads + (size_t)idx * st->payload_size, payload, st->payload_size);
    }
    st->slot_samples[idx] = num_samples;
    st->slot_kind[idx]    = is_parity ? 2 : 1;

    /* If we just filled the parity slot, the group is complete - try
     * recovery, then flush the whole window. */
    if (offset == st->window_size - 1) {
        try_recover_group(st);
        for (int i = 0; i < st->window_size; i++) emit_head_slot(st, emit, ud);
    }
}

void fec_rx_flush(FecRxState *st, FecRxEmitCb emit, void *ud) {
    if (!st->enabled) return;
    try_recover_group(st);
    for (int i = 0; i < st->window_size; i++) emit_head_slot(st, emit, ud);
}

/* Expose to translation unit only - used by fec_tx_on_data when a parity
 * was returned to the caller. Keep it static, the public API will call
 * it from the inline `emitted` path. */
static void fec_tx_clear_after_emit_(FecTxState *st) {
    fec_tx_reset_after_emit(st);
}

/* The header advertised fec_tx_reset_after_emit indirectly. The
 * canonical contract: after the caller sends the parity, they must call
 * fec_tx_clear_after_emit which the udp_tx module wraps. */
void fec_tx_clear_after_emit(FecTxState *st) {
    fec_tx_clear_after_emit_(st);
}
