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
#include <string.h>

static uint8_t *alloc_packet_buf(int channels, int bit_depth, int buffer_size) {
    int bps;
    if (bit_depth == 16)      bps = 2;
    else if (bit_depth == 24) bps = 3;
    else                      bps = 4;
    size_t cap = (size_t)buffer_size * (size_t)channels * (size_t)bps + PCM_HEADER_SIZE;
    return (uint8_t *)calloc(1, cap);
}

int udp_sender_init(UdpSender *tx, const char *target_addr, uint16_t port,
                    const char *iface_addr, bool multicast,
                    int sample_rate, int channels, int bit_depth,
                    int buffer_size) {
    init_winsock();
    memset(tx, 0, sizeof(*tx));

    tx->sample_rate = sample_rate;
    tx->channels    = channels;
    tx->bit_depth   = bit_depth;
    tx->buffer_size = buffer_size;
    tx->sequence    = 0;
    tx->timestamp   = 0;

    tx->packet_buf = alloc_packet_buf(channels, bit_depth, buffer_size);
    if (!tx->packet_buf) {
        fprintf(stderr, "udp_sender_init: failed to allocate packet buffer\n");
        return -1;
    }
    tx->packet_capacity = (size_t)buffer_size * (size_t)channels * (size_t)(bit_depth == 16 ? 2 : (bit_depth == 24 ? 3 : 4)) + PCM_HEADER_SIZE;

    memset(&tx->target_addr, 0, sizeof(tx->target_addr));
    tx->target_addr.sin_family = AF_INET;
    tx->target_addr.sin_port   = htons(port);
    tx->target_addr.sin_addr.s_addr = inet_addr(target_addr ? target_addr : "239.77.77.77");

    tx->sock = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (tx->sock < 0) {
        perror("socket");
        return -1;
    }

    if (multicast) {
        unsigned char ttl = 32;
        if (setsockopt(tx->sock, IPPROTO_IP, IP_MULTICAST_TTL,
                       (const char *)&ttl, sizeof(ttl)) < 0) {
            perror("setsockopt IP_MULTICAST_TTL");
        }

        if (iface_addr && strcmp(iface_addr, "0.0.0.0") != 0) {
            struct in_addr iface;
            iface.s_addr = inet_addr(iface_addr);
            if (setsockopt(tx->sock, IPPROTO_IP, IP_MULTICAST_IF,
                           (const char *)&iface, sizeof(iface)) < 0) {
                perror("setsockopt IP_MULTICAST_IF");
            }
        }
    }

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
    if (tx->sock >= 0) {
        close_socket(tx->sock);
        tx->sock = -1;
    }
    free(tx->packet_buf);
    tx->packet_buf = NULL;
    tx->running = false;
}

int udp_sender_send(UdpSender *tx, const int32_t * const *channels,
                    int num_channels, int samples) {
    if (!tx->running) return -1;

    pcm_header_write(tx->packet_buf, tx->sequence, tx->timestamp,
                     sample_rate_to_rate_id((unsigned int)tx->sample_rate),
                     num_channels, tx->bit_depth, (uint16_t)samples);

    size_t pcm_capacity = tx->packet_capacity - PCM_HEADER_SIZE;
    size_t pcm_len = pcm_pack(channels, num_channels, samples,
                              tx->bit_depth, tx->packet_buf + PCM_HEADER_SIZE,
                              pcm_capacity);
    if (pcm_len == 0) return -1;

    size_t total_len = PCM_HEADER_SIZE + pcm_len;

    int sent = (int)sendto(tx->sock, (const char *)tx->packet_buf, (int)total_len, 0,
                           (struct sockaddr *)&tx->target_addr,
                           sizeof(tx->target_addr));
    if (sent < 0) {
        int e = sockerr;
        if (e != EWOULDBLOCK_ERR) {
            return -1;
        }
        return 0;
    }

    tx->sequence++;
    tx->timestamp += samples;
    return (int)sent;
}
