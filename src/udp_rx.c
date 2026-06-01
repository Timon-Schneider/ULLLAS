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

static int parse_ipv4(const char *s, struct in_addr *out) {
    if (!s || !*s) {
        out->s_addr = htonl(INADDR_ANY);
        return 0;
    }
    return inet_pton(AF_INET, s, out) == 1 ? 0 : -1;
}

static void set_socket_rcvbuf(ulllas_socket_t s) {
    int rcvbuf = 2 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
    int tos = 0xB8;
    setsockopt(s, IPPROTO_IP, IP_TOS, (const char *)&tos, sizeof(tos));
}

int udp_receiver_init(UdpReceiver *rx, const char *bind_addr, uint16_t port, const char *mcast_addr,
                      const char *iface_addr, bool multicast, int channels, int bit_depth,
                      int max_samples_per_packet) {
    init_winsock();
    memset(rx, 0, sizeof(*rx));
    rx->sock = ULLLAS_INVALID_SOCKET;

    rx->channels               = channels;
    rx->bit_depth              = bit_depth;
    rx->max_samples_per_packet = max_samples_per_packet;

    rx->packet_buf_size = ULLLAS_MTU_PAYLOAD + 64; /* slack */
    rx->packet_buf      = (uint8_t *)calloc(1, rx->packet_buf_size);
    if (!rx->packet_buf) {
        fprintf(stderr, "udp_receiver_init: failed to allocate packet buffer\n");
        return -1;
    }

    rx->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (!ULLLAS_SOCK_VALID(rx->sock)) {
        perror("socket");
        free(rx->packet_buf);
        rx->packet_buf = NULL;
        return -1;
    }

    int reuse = 1;
    setsockopt(rx->sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    set_socket_rcvbuf(rx->sock);

    memset(&rx->bind_addr, 0, sizeof(rx->bind_addr));
    rx->bind_addr.sin_family = AF_INET;
    rx->bind_addr.sin_port   = htons(port);
    if (parse_ipv4(bind_addr, &rx->bind_addr.sin_addr) != 0) {
        fprintf(stderr, "udp_receiver_init: invalid bind address '%s'\n", bind_addr ? bind_addr : "(null)");
        close_socket(rx->sock);
        rx->sock = ULLLAS_INVALID_SOCKET;
        free(rx->packet_buf);
        rx->packet_buf = NULL;
        return -1;
    }

    if (bind(rx->sock, (struct sockaddr *)&rx->bind_addr, sizeof(rx->bind_addr)) < 0) {
        perror("bind");
        close_socket(rx->sock);
        rx->sock = ULLLAS_INVALID_SOCKET;
        free(rx->packet_buf);
        rx->packet_buf = NULL;
        return -1;
    }

    if (multicast && mcast_addr && mcast_addr[0] != '\0') {
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        if (parse_ipv4(mcast_addr, &mreq.imr_multiaddr) != 0) {
            fprintf(stderr, "udp_receiver_init: invalid multicast address '%s'\n", mcast_addr);
            close_socket(rx->sock);
            rx->sock = ULLLAS_INVALID_SOCKET;
            free(rx->packet_buf);
            rx->packet_buf = NULL;
            return -1;
        }
        /* Prefer explicit --iface for multi-homed hosts. Falls back to
         * bind_addr (default 0.0.0.0). */
        if (iface_addr && iface_addr[0] && strcmp(iface_addr, "0.0.0.0") != 0) {
            if (parse_ipv4(iface_addr, &mreq.imr_interface) != 0) {
                fprintf(stderr, "udp_receiver_init: invalid iface '%s'\n", iface_addr);
            }
        } else {
            mreq.imr_interface = rx->bind_addr.sin_addr;
        }
        if (setsockopt(rx->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mreq, sizeof(mreq)) < 0) {
            perror("setsockopt IP_ADD_MEMBERSHIP");
        }
    }

    /* Blocking socket with a short timeout. recv returns 0 on timeout
     * which lets the caller observe the shutdown flag and exit
     * cleanly. */
#ifdef _WIN32
    u_long block = 0;
    ioctlsocket(rx->sock, FIONBIO, &block);
    DWORD timeout_ms = 200;
    setsockopt(rx->sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
#else
    int flags = fcntl(rx->sock, F_GETFL, 0);
    fcntl(rx->sock, F_SETFL, flags & ~O_NONBLOCK);
    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 200000;
    setsockopt(rx->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    rx->running = true;
    return 0;
}

void udp_receiver_destroy(UdpReceiver *rx) {
    if (ULLLAS_SOCK_VALID(rx->sock)) {
        close_socket(rx->sock);
        rx->sock = ULLLAS_INVALID_SOCKET;
    }
    free(rx->packet_buf);
    rx->packet_buf = NULL;
    rx->running    = false;
}

int udp_receiver_recv(UdpReceiver *rx, PcmHeader *out_hdr, const uint8_t **out_payload, size_t *out_payload_len) {
    if (!rx->running) return -1;

    struct sockaddr_in from;
#ifdef _WIN32
    int fromlen = sizeof(from);
#else
    socklen_t fromlen = sizeof(from);
#endif

    int recvd = (int)recvfrom(rx->sock, (char *)rx->packet_buf, (int)rx->packet_buf_size, 0,
                              (struct sockaddr *)&from, &fromlen);
    if (recvd < 0) {
        int e = sockerr;
#ifdef _WIN32
        if (e == EWOULDBLOCK_ERR || e == WSAETIMEDOUT) return 0;
#else
        if (e == EWOULDBLOCK_ERR || e == EAGAIN) return 0;
#endif
        return -1;
    }
    if (recvd < (int)PCM_HEADER_SIZE) return 0;

    PcmHeader hdr;
    if (!pcm_header_read(rx->packet_buf, &hdr)) {
        /* Stray packet on our port. Silent drop. */
        return 0;
    }

    if ((int)hdr.channels != rx->channels) {
        /* Don't fprintf from this hot path. The status loop reports
         * any large discrepancy through the loss counters instead. */
        return 0;
    }

    *out_hdr         = hdr;
    *out_payload     = rx->packet_buf + PCM_HEADER_SIZE;
    *out_payload_len = (size_t)recvd - PCM_HEADER_SIZE;
    return recvd;
}
