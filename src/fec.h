#ifndef ULLLAS_FEC_H
#define ULLLAS_FEC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Simple XOR forward error correction.
 *
 * The wire layout uses a global sequence number; every (N+1)th packet
 * is a parity packet whose payload is the XOR of the previous N data
 * packets. With group_size=4, the cycle is D,D,D,D,P,D,D,D,D,P,...
 * Packet kind is encoded in PcmHeader.flags (bit 0).
 *
 * Constraint (enforced at sender init): all data packets in a stream
 * must have the same payload size when FEC is on. The sender refuses
 * to start otherwise.
 *
 * On the receive side a sliding window of N+1 packets is maintained
 * before flushing to the ring buffer. This is what makes recovery
 * effective for audio: by the time a parity arrives, the data packets
 * it covers are still in the window and any single missing one can be
 * filled in. The cost is N+1 packets of added playback latency.
 */

#define FEC_MAX_GROUP 16

/* ---------- sender ---------- */

typedef struct {
    int      enabled;
    int      group_size;   /* N */
    size_t   payload_size; /* bytes per data payload */
    uint8_t *xor_buf;
    int      received_in_group;
} FecTxState;

int  fec_tx_init(FecTxState *st, int group_size, size_t payload_size);
void fec_tx_destroy(FecTxState *st);

/* Returns 1 if the caller should now send a parity packet whose payload
 * is fec_tx_parity_buf(st). After sending the parity the caller must
 * call fec_tx_clear_after_emit() so the accumulator is reset for the
 * next group. */
int            fec_tx_on_data(FecTxState *st, const uint8_t *payload, size_t payload_len);
const uint8_t *fec_tx_parity_buf(const FecTxState *st);
void           fec_tx_clear_after_emit(FecTxState *st);

/* ---------- receiver ---------- */

typedef void (*FecRxEmitCb)(uint32_t seq, uint16_t num_samples, const uint8_t *payload, int recovered, void *userdata);

typedef struct {
    int      enabled;
    int      group_size;
    int      window_size; /* N+1 */
    size_t   payload_size;
    uint16_t expected_num_samples; /* learned from first data packet */

    /* Window ring: window_size slots. Each slot holds one packet's
     * payload + metadata + a kind tag (data/parity/empty). */
    uint8_t  *payloads;     /* window_size * payload_size */
    uint8_t  *slot_kind;    /* 0=empty, 1=data, 2=parity */
    uint16_t *slot_samples; /* num_samples per slot */

    uint32_t head_seq; /* seq of the oldest slot (slot 0) */
    int      head_idx; /* slot index of head (rotates) */
    int      started;  /* whether head_seq is meaningful yet */

    uint64_t recovered_count;
    uint64_t unrecoverable_count;
} FecRxState;

int  fec_rx_init(FecRxState *st, int group_size, size_t payload_size);
void fec_rx_destroy(FecRxState *st);

/* Feed an incoming packet to the FEC receive logic. Any data packets
 * that become ready (because they're at the head of the window) are
 * emitted via `emit` in seq order. `emit` may also be called with
 * NULL payload when a slot is irrecoverably lost (caller fills via
 * PLC). */
void fec_rx_process(FecRxState *st, uint32_t seq, int is_parity, uint16_t num_samples, const uint8_t *payload,
                    FecRxEmitCb emit, void *userdata);

/* Flush all remaining slots in the window (called on shutdown or stream
 * restart). */
void fec_rx_flush(FecRxState *st, FecRxEmitCb emit, void *userdata);

void fec_rx_reset(FecRxState *st);

#ifdef __cplusplus
}
#endif

#endif
