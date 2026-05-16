#include "network.h"
#include "pcm_proto.h"

#ifdef _WIN32
    #include <ws2tcpip.h>
    #ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")
    #endif
    #define close_socket closesocket
    #define sockerr WSAGetLastError()
    #ifndef EWOULDBLOCK_ERR
        #define EWOULDBLOCK_ERR WSAEWOULDBLOCK
    #endif
    #define socklen_t_int int
    static int winsock_inited = 0;
    static void init_winsock(void) {
        if (!winsock_inited) {
            WSADATA wsa;
            WSAStartup(MAKEWORD(2, 2), &wsa);
            winsock_inited = 1;
        }
    }
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <string.h>
    #define close_socket close
    #define sockerr errno
    #ifndef EWOULDBLOCK_ERR
        #define EWOULDBLOCK_ERR EWOULDBLOCK
    #endif
    #define socklen_t_int socklen_t
    #define init_winsock() ((void)0)
#endif

#include <stdio.h>
#include <stdlib.h>

int udp_receiver_init(UdpReceiver *rx, const char *bind_addr, uint16_t port,
                      const char *mcast_addr, bool multicast,
                      int channels, int bit_depth, int buffer_size,
                      unsigned int jitter_packets) {
    init_winsock();
    memset(rx, 0, sizeof(*rx));

    rx->channels        = channels;
    rx->bit_depth       = bit_depth;
    rx->buffer_size     = buffer_size;
    rx->jitter_packets  = jitter_packets;
    rx->expected_seq    = 0;

    rx->packet_capacity = (size_t)4096 * (size_t)channels * (size_t)4 + PCM_HEADER_SIZE;
    rx->packet_buf = (uint8_t *)calloc(1, rx->packet_capacity);
    if (!rx->packet_buf) {
        fprintf(stderr, "udp_receiver_init: failed to allocate packet buffer\n");
        return -1;
    }

    rx->sock = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (rx->sock < 0) {
        perror("socket");
        return -1;
    }

    int reuse = 1;
    if (setsockopt(rx->sock, SOL_SOCKET, SO_REUSEADDR,
                   (const char *)&reuse, sizeof(reuse)) < 0) {
        perror("setsockopt SO_REUSEADDR");
    }

    memset(&rx->bind_addr, 0, sizeof(rx->bind_addr));
    rx->bind_addr.sin_family      = AF_INET;
    rx->bind_addr.sin_port        = htons(port);
    rx->bind_addr.sin_addr.s_addr = bind_addr ? inet_addr(bind_addr) : INADDR_ANY;

    if (bind(rx->sock, (struct sockaddr *)&rx->bind_addr, sizeof(rx->bind_addr)) < 0) {
        perror("bind");
        close_socket(rx->sock);
        rx->sock = -1;
        return -1;
    }

    if (multicast && mcast_addr && mcast_addr[0] != '\0') {
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = inet_addr(mcast_addr);
        mreq.imr_interface.s_addr = bind_addr ? inet_addr(bind_addr) : INADDR_ANY;
        if (setsockopt(rx->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       (const char *)&mreq, sizeof(mreq)) < 0) {
            perror("setsockopt IP_ADD_MEMBERSHIP");
        }
    }

#ifdef _WIN32
    u_long nonblock = 1;
    ioctlsocket(rx->sock, FIONBIO, &nonblock);
#else
    int flags = fcntl(rx->sock, F_GETFL, 0);
    fcntl(rx->sock, F_SETFL, flags | O_NONBLOCK);
#endif

    rx->running = true;
    return 0;
}

void udp_receiver_destroy(UdpReceiver *rx) {
    if (rx->sock >= 0) {
        close_socket(rx->sock);
        rx->sock = -1;
    }
    free(rx->packet_buf);
    rx->packet_buf = NULL;
    rx->running = false;
}

int udp_receiver_recv(UdpReceiver *rx, int32_t **channels, int max_samples,
                      uint32_t *out_seq, uint64_t *out_timestamp) {
    if (!rx->running) return -1;

    struct sockaddr_in from;
#ifdef _WIN32
    int fromlen = sizeof(from);
#else
    socklen_t fromlen = sizeof(from);
#endif

    int recvd = (int)recvfrom(rx->sock, (char *)rx->packet_buf, (int)rx->packet_capacity, 0,
                              (struct sockaddr *)&from, &fromlen);
    if (recvd < 0) {
        int e = sockerr;
#ifdef _WIN32
        if (e == EWOULDBLOCK_ERR || e == WSAETIMEDOUT) return 0;
#else
        if (e == EWOULDBLOCK_ERR || e == EAGAIN) return 0;
#endif
        perror("recvfrom");
        return -1;
    }

    if (recvd < (int)PCM_HEADER_SIZE) return 0;

    uint32_t seq;
    uint64_t timestamp;
    int rate_id, ch_count, bit_depth;
    uint16_t num_samples;
    pcm_header_read(rx->packet_buf, &seq, &timestamp, &rate_id, &ch_count, &bit_depth, &num_samples);

    if (ch_count != rx->channels) {
        fprintf(stderr, "rx: channel mismatch: got %d, expected %d\n", ch_count, rx->channels);
        return 0;
    }

    if (num_samples > 0) {
        int hdr_bps;
        if (bit_depth == 16)      hdr_bps = 2;
        else if (bit_depth == 24) hdr_bps = 3;
        else                      hdr_bps = 4;
        size_t expected = PCM_HEADER_SIZE + (size_t)num_samples * (size_t)ch_count * (size_t)hdr_bps;
        if ((size_t)recvd < expected) {
            fprintf(stderr, "rx: truncated packet (got %d bytes, expected %zu, sender sent %u samples)\n",
                    recvd, expected, num_samples);
        }
    }

    if (rx->expected_seq > 0 && seq != rx->expected_seq) {
        if (seq > rx->expected_seq) {
            fprintf(stderr, "rx: packet loss: %d missing (expected %u, got %u)\n",
                    seq - rx->expected_seq, rx->expected_seq, seq);
        }
    }
    rx->expected_seq = seq + 1;

    size_t pcm_bytes = (size_t)recvd - PCM_HEADER_SIZE;
    int samples = pcm_unpack(rx->packet_buf + PCM_HEADER_SIZE, pcm_bytes,
                             ch_count, bit_depth, channels, max_samples);

    if (num_samples > 0 && samples < (int)num_samples) {
        fprintf(stderr, "rx: buffer too small for packet (unpacked %d of %u samples, max_samples=%d)\n",
                samples, num_samples, max_samples);
    }

    if (out_seq)       *out_seq       = seq;
    if (out_timestamp) *out_timestamp = timestamp;

    return samples;
}
