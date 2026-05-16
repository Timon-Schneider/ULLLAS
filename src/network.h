#ifndef ULLLAS_NETWORK_H
#define ULLLAS_NETWORK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sock;
    struct sockaddr_in target_addr;
    uint8_t *packet_buf;
    size_t  packet_capacity;
    uint32_t sequence;
    uint64_t timestamp;
    int sample_rate;
    int channels;
    int bit_depth;
    int buffer_size;
    bool running;
} UdpSender;

typedef struct {
    int sock;
    struct sockaddr_in bind_addr;
    uint8_t *packet_buf;
    size_t  packet_capacity;
    int channels;
    int bit_depth;
    int buffer_size;
    unsigned int jitter_packets;
    uint32_t expected_seq;
    bool running;
} UdpReceiver;

int udp_sender_init(UdpSender *tx, const char *target_addr, uint16_t port,
                    const char *iface_addr, bool multicast,
                    int sample_rate, int channels, int bit_depth,
                    int buffer_size);

void udp_sender_destroy(UdpSender *tx);

int udp_sender_send(UdpSender *tx, const int32_t * const *channels,
                    int num_channels, int samples);

int udp_receiver_init(UdpReceiver *rx, const char *bind_addr, uint16_t port,
                      const char *mcast_addr, bool multicast,
                      int channels, int bit_depth, int buffer_size,
                      unsigned int jitter_packets);

void udp_receiver_destroy(UdpReceiver *rx);

int udp_receiver_recv(UdpReceiver *rx, int32_t **channels, int max_samples,
                      uint32_t *out_seq, uint64_t *out_timestamp);

#ifdef __cplusplus
}
#endif

#endif
