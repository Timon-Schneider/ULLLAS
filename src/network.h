#ifndef ULLLAS_NETWORK_H
#define ULLLAS_NETWORK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "pcm_proto.h"
#include "fec.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET ulllas_socket_t;
#define ULLLAS_INVALID_SOCKET INVALID_SOCKET
#define ULLLAS_SOCK_VALID(s)  ((s) != INVALID_SOCKET)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
typedef int ulllas_socket_t;
#define ULLLAS_INVALID_SOCKET (-1)
#define ULLLAS_SOCK_VALID(s)  ((s) >= 0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Conservative IPv4 / Ethernet payload budget: 1500 - 20 (IP) - 8 (UDP). */
#define ULLLAS_MTU_PAYLOAD 1472

typedef struct {
    ulllas_socket_t    sock;
    struct sockaddr_in target_addr;

    uint8_t *packet_buf; /* single allocation, sized for max MTU packet */

    /* Stream config (constant after init). */
    int sample_rate;
    int channels;
    int bit_depth;
    int max_samples_per_packet; /* derived from MTU + bit_depth + channels */

    /* Running state. */
    uint32_t sequence;
    uint64_t timestamp; /* monotonic frame counter */

    /* FEC. */
    FecTxState fec;
    int        fec_group_size; /* 0 = off */

    /* Stats (incremented from audio callback - readers should use
     * atomic loads; here we keep them plain since only the audio
     * callback writes and only the status thread reads, and stale
     * values are fine). */
    volatile uint64_t total_packets_tx;
    volatile uint64_t total_send_errors;
    volatile uint64_t total_fec_parity_tx;
    volatile uint64_t total_frames_tx;

    bool running;
} UdpSender;

typedef struct {
    ulllas_socket_t    sock;
    struct sockaddr_in bind_addr;

    uint8_t *packet_buf;
    size_t   packet_buf_size;

    int channels;
    int bit_depth;
    int max_samples_per_packet;

    bool running;
} UdpReceiver;

int  udp_sender_init(UdpSender *tx, const char *target_addr, uint16_t port, const char *iface_addr, bool multicast,
                     int sample_rate, int channels, int bit_depth, int samples_per_audio_buffer, int fec_group_size);
void udp_sender_destroy(UdpSender *tx);

/* Pack and send one audio block. Splits into multiple MTU-sized packets
 * if needed. Safe to call from a real-time audio callback. Returns
 * 0 on success, -1 on hard error. EWOULDBLOCK is silently dropped and
 * counted as a send error (not -1).
 *
 * NOTE: when FEC is enabled the implementation rejects calls where
 * `samples` exceeds max_samples_per_packet, because FEC requires all
 * data packets to be the same size. The sender init catches this case
 * up front, so a properly configured caller never trips it. */
int udp_sender_send_planar(UdpSender *tx, const int32_t *const *channels, int channels_count, int samples);

int  udp_receiver_init(UdpReceiver *rx, const char *bind_addr, uint16_t port, const char *mcast_addr,
                       const char *iface_addr, bool multicast, int channels, int bit_depth,
                       int max_samples_per_packet);
void udp_receiver_destroy(UdpReceiver *rx);

/* Receive one packet. Returns:
 *   >0  = success; out_hdr is filled and out_payload / out_payload_len
 *         point into the receiver's internal buffer, valid until the
 *         next call.
 *   0   = timeout / would-block / packet rejected (bad magic, truncated,
 *         channel mismatch). The caller should just call again.
 *  <0   = hard error. */
int udp_receiver_recv(UdpReceiver *rx, PcmHeader *out_hdr, const uint8_t **out_payload, size_t *out_payload_len);

/* Compute the maximum number of frames that fits in a single MTU-sized
 * packet for the given format. Returns 0 on bad arguments. */
int udp_compute_max_samples_per_packet(int channels, int bit_depth);

#ifdef __cplusplus
}
#endif

#endif
