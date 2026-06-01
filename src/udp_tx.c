#include "network.h"
#include "pcm_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#define close_socket closesocket
#define sockerr      WSAGetLastError()
#ifndef EWOULDBLOCK_ERR
#define EWOULDBLOCK_ERR WSAEWOULDBLOCK
#endif
static int winsock_inited = 0;
static void init_winsock(void) {
    if (!winsock_inited) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        winsock_inited = 1;
    }
}
#else
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#define close_socket   close
#define sockerr        errno
#ifndef EWOULDBLOCK_ERR
#define EWOULDBLOCK_ERR EWOULDBLOCK
#endif
#define init_winsock() ((void)0)
#endif

int udp_compute_max_samples_per_packet(int channels, int bit_depth) {
    if (channels <= 0) return 0;
    int    bps        = pcm_bytes_per_sample(bit_depth);
    size_t frame_size = (size_t)bps * (size_t)channels;
    if (frame_size == 0) return 0;
    size_t payload_capacity = ULLLAS_MTU_PAYLOAD - PCM_HEADER_SIZE;
    int    max_samples      = (int)(payload_capacity / frame_size);
    if (max_samples < 1) return 0;
    return max_samples;
}

static int parse_ipv4(const char *s, struct in_addr *out) {
    if (!s || !*s) {
        out->s_addr = htonl(INADDR_ANY);
        return 0;
    }
    return inet_pton(AF_INET, s, out) == 1 ? 0 : -1;
}

static void set_socket_low_latency(ulllas_socket_t s, int is_sender) {
    /* DSCP EF (0xB8 = 184) on the IP header. Many managed LAN switches
     * use this to prioritize traffic. */
    int tos = 0xB8;
    setsockopt(s, IPPROTO_IP, IP_TOS, (const char *)&tos, sizeof(tos));

    if (is_sender) {
        /* A small send buffer fails fast under congestion instead of
         * silently delaying packets by hundreds of ms. We still allow
         * enough room for a few packets to coalesce. */
        int sndbuf = 256 * 1024;
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf));
    } else {
        /* Receivers want headroom for scheduler stalls. */
        int rcvbuf = 2 * 1024 * 1024;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
    }

#ifdef _WIN32
    /* Windows: IP_DONTFRAGMENT lives under IPPROTO_IP. */
#ifdef IP_DONTFRAGMENT
    DWORD df = 1;
    setsockopt(s, IPPROTO_IP, IP_DONTFRAGMENT, (const char *)&df, sizeof(df));
#endif
#else
#ifdef IP_MTU_DISCOVER
    /* Linux: enable PMTUD which sets DF and rejects oversized packets. */
    int mtu = IP_PMTUDISC_DO;
    setsockopt(s, IPPROTO_IP, IP_MTU_DISCOVER, &mtu, sizeof(mtu));
#elif defined(IP_DONTFRAG)
    /* macOS / BSD. */
    int df = 1;
    setsockopt(s, IPPROTO_IP, IP_DONTFRAG, &df, sizeof(df));
#endif
#endif
}

int udp_sender_init(UdpSender *tx, const char *target_addr, uint16_t port, const char *iface_addr, bool multicast,
                    int sample_rate, int channels, int bit_depth, int samples_per_audio_buffer, int fec_group_size) {
    init_winsock();
    memset(tx, 0, sizeof(*tx));
    tx->sock = ULLLAS_INVALID_SOCKET;

    tx->sample_rate = sample_rate;
    tx->channels    = channels;
    tx->bit_depth   = bit_depth;

    int max_per_packet = udp_compute_max_samples_per_packet(channels, bit_depth);
    if (max_per_packet <= 0) {
        fprintf(stderr, "udp_sender_init: invalid channels/bit_depth\n");
        return -1;
    }
    tx->max_samples_per_packet = max_per_packet;

    if (fec_group_size > 0) {
        /* FEC requires all data packets to be the same size, so we
         * forbid in-buffer splitting when FEC is on. */
        if (samples_per_audio_buffer > max_per_packet) {
            fprintf(stderr,
                    "udp_sender_init: FEC is enabled but the audio buffer (%d samples) does not fit in one MTU "
                    "packet at this format (max %d). Either reduce --buffer, lower --bit-depth, or disable --fec.\n",
                    samples_per_audio_buffer, max_per_packet);
            return -1;
        }
        size_t payload_size = (size_t)samples_per_audio_buffer * (size_t)channels * (size_t)pcm_bytes_per_sample(bit_depth);
        if (fec_tx_init(&tx->fec, fec_group_size, payload_size) != 0) {
            fprintf(stderr, "udp_sender_init: fec_tx_init failed\n");
            return -1;
        }
        tx->fec_group_size = fec_group_size;
    }

    size_t buf_size = ULLLAS_MTU_PAYLOAD;
    tx->packet_buf  = (uint8_t *)calloc(1, buf_size);
    if (!tx->packet_buf) {
        fec_tx_destroy(&tx->fec);
        return -1;
    }

    memset(&tx->target_addr, 0, sizeof(tx->target_addr));
    tx->target_addr.sin_family = AF_INET;
    tx->target_addr.sin_port   = htons(port);
    if (parse_ipv4(target_addr ? target_addr : "239.77.77.77", &tx->target_addr.sin_addr) != 0) {
        fprintf(stderr, "udp_sender_init: invalid target address '%s'\n", target_addr ? target_addr : "(null)");
        free(tx->packet_buf);
        fec_tx_destroy(&tx->fec);
        return -1;
    }

    tx->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (!ULLLAS_SOCK_VALID(tx->sock)) {
        perror("socket");
        free(tx->packet_buf);
        fec_tx_destroy(&tx->fec);
        return -1;
    }

    set_socket_low_latency(tx->sock, /*is_sender=*/1);

    if (multicast) {
        unsigned char ttl = 32;
        setsockopt(tx->sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl));

        /* Disable loopback - the sender doesn't need to hear itself.
         * This avoids extra kernel work on every send. */
        unsigned char loop = 0;
        setsockopt(tx->sock, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *)&loop, sizeof(loop));

        if (iface_addr && strcmp(iface_addr, "0.0.0.0") != 0) {
            struct in_addr iface;
            if (parse_ipv4(iface_addr, &iface) == 0) {
                setsockopt(tx->sock, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&iface, sizeof(iface));
            } else {
                fprintf(stderr, "udp_sender_init: invalid iface '%s'\n", iface_addr);
            }
        }
    }

    /* Non-blocking so the audio callback never stalls on a full send
     * buffer. EWOULDBLOCK from sendto is treated as a dropped packet. */
#ifdef _WIN32
    u_long nonblock = 1;
    ioctlsocket(tx->sock, FIONBIO, &nonblock);
#else
    int flags = fcntl(tx->sock, F_GETFL, 0);
    fcntl(tx->sock, F_SETFL, flags | O_NONBLOCK);
#endif

    tx->running = true;
    return 0;
}

void udp_sender_destroy(UdpSender *tx) {
    if (ULLLAS_SOCK_VALID(tx->sock)) {
        close_socket(tx->sock);
        tx->sock = ULLLAS_INVALID_SOCKET;
    }
    free(tx->packet_buf);
    tx->packet_buf = NULL;
    fec_tx_destroy(&tx->fec);
    tx->running = false;
}

static int send_one(UdpSender *tx, const uint8_t *buf, size_t len) {
    int sent = (int)sendto(tx->sock, (const char *)buf, (int)len, 0, (struct sockaddr *)&tx->target_addr,
                            sizeof(tx->target_addr));
    if (sent < 0) {
        int e = sockerr;
        if (e == EWOULDBLOCK_ERR) {
            tx->total_send_errors++;
            return 0;
        }
        tx->total_send_errors++;
        return -1;
    }
    return sent;
}

static int emit_data_packet(UdpSender *tx, const int32_t *const *channels, int channels_count, int samples,
                            int sample_offset) {
    PcmHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic          = PCM_PROTO_MAGIC;
    hdr.seq            = tx->sequence;
    hdr.timestamp      = tx->timestamp + (uint64_t)sample_offset;
    hdr.bit_depth      = (uint8_t)tx->bit_depth;
    hdr.channels       = (uint8_t)channels_count;
    hdr.num_samples    = (uint16_t)samples;
    hdr.sample_rate    = (uint32_t)tx->sample_rate;
    hdr.flags          = 0;
    hdr.fec_group_size = (uint8_t)tx->fec_group_size;

    pcm_header_write(tx->packet_buf, &hdr);

    /* Pack a planar slice into the packet payload. We index each
     * channel pointer at +sample_offset. */
    const int32_t *slice_ptrs[256];
    if (channels_count < 0 || channels_count > 255) return -1;
    for (int ch = 0; ch < channels_count; ch++) {
        slice_ptrs[ch] = channels[ch] + sample_offset;
    }

    size_t payload_capacity = ULLLAS_MTU_PAYLOAD - PCM_HEADER_SIZE;
    size_t pcm_len          = pcm_pack((const int32_t *const *)slice_ptrs, channels_count, samples, tx->bit_depth,
                              tx->packet_buf + PCM_HEADER_SIZE, payload_capacity);
    if (pcm_len == 0) return -1;

    size_t total = PCM_HEADER_SIZE + pcm_len;

    int rc = send_one(tx, tx->packet_buf, total);
    if (rc < 0) return -1;

    tx->total_packets_tx++;
    tx->total_frames_tx += (uint64_t)samples;
    tx->sequence++;

    /* Feed the data payload into the FEC accumulator, and emit a
     * parity packet if the group just closed. */
    if (tx->fec_group_size > 0) {
        int emit = fec_tx_on_data(&tx->fec, tx->packet_buf + PCM_HEADER_SIZE, pcm_len);
        if (emit) {
            PcmHeader p = hdr;
            p.seq       = tx->sequence;
            p.flags     = PCM_FLAG_PARITY;
            p.timestamp = tx->timestamp + (uint64_t)sample_offset;
            pcm_header_write(tx->packet_buf, &p);
            memcpy(tx->packet_buf + PCM_HEADER_SIZE, fec_tx_parity_buf(&tx->fec), pcm_len);
            int rcp = send_one(tx, tx->packet_buf, total);
            fec_tx_clear_after_emit(&tx->fec);
            if (rcp >= 0) {
                tx->total_packets_tx++;
                tx->total_fec_parity_tx++;
            }
            tx->sequence++;
        }
    }

    return 0;
}

int udp_sender_send_planar(UdpSender *tx, const int32_t *const *channels, int channels_count, int samples) {
    if (!tx->running) return -1;
    if (samples <= 0 || channels_count <= 0) return -1;

    int max_per_pkt = tx->max_samples_per_packet;
    int offset      = 0;
    while (offset < samples) {
        int chunk = samples - offset;
        if (chunk > max_per_pkt) chunk = max_per_pkt;
        if (emit_data_packet(tx, channels, channels_count, chunk, offset) < 0) {
            /* Hard error - keep going so audio doesn't stall, but
             * count it. */
        }
        offset += chunk;
    }
    tx->timestamp += (uint64_t)samples;
    return 0;
}
