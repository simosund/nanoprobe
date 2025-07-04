#ifndef NANOPING_H
#define NANOPING_H

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>
#include <linux/udp.h>
#include <linux/ip.h>
#include <linux/if_ether.h>

#define NS_PER_S 1000000000
#define NS_PER_US 1000

#define MAX_PAD_BYTES 2048

#define TOT_NETHDR_SIZE (sizeof(struct udphdr) + sizeof(struct iphdr))
#define TOT_LINKHDR_SIZE (TOT_NETHDR_SIZE + sizeof(struct ethhdr))
#define TOT_APPNETHDR_SIZE (sizeof(struct nanoping_msg) +  TOT_NETHDR_SIZE)

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))

#define min(a,b)                                \
    ({ typeof (a) _a = (a);                     \
        typeof (b) _b = (b);                    \
        _a < _b ? _a : _b; })

#define timevaladd(tvp, uvp, vvp) \
    do { \
        (vvp)->tv_sec = (tvp)->tv_sec + (uvp)->tv_sec; \
        (vvp)->tv_nsec = (tvp)->tv_nsec + (uvp)->tv_nsec; \
        if ((vvp)->tv_nsec >= 1000000000) { \
            (vvp)->tv_sec++; \
            (vvp)->tv_nsec -= 1000000000; \
        } \
    } while (0)

#define timevalsub(tvp, uvp, vvp) \
    do { \
        assert(!timevalcmp(tvp, uvp, <)); \
        (vvp)->tv_sec = (tvp)->tv_sec - (uvp)->tv_sec; \
        (vvp)->tv_nsec = (tvp)->tv_nsec - (uvp)->tv_nsec; \
        if ((vvp)->tv_nsec < 0) { \
            (vvp)->tv_sec--; \
            (vvp)->tv_nsec += 1000000000; \
        } \
    } while (0)

#define timevalcmp(tvp, uvp, cmp) \
    (((tvp)->tv_sec == (uvp)->tv_sec) ? \
    ((tvp)->tv_nsec cmp (uvp)->tv_nsec) : \
    ((tvp)->tv_sec cmp (uvp)->tv_sec))

struct nanoping_instance {
    int fd;
    int nots_fd;
    struct sockaddr_in myaddr;
    bool server;
    bool emulation;
    int emul_fds[2];
    FILE *log_stream;
    unsigned long pkt_received;
    unsigned long pkt_transmitted;
    unsigned long rxs_collected;
    unsigned long txs_collected;
    uint64_t sent_seq;
    bool log_pktdir;
};

enum nanoping_msg_type {
    msg_none = 0,
    msg_syn,
    msg_syn_ack,
    msg_syn_rst,
    msg_ping_wait,
    msg_ping,
    msg_pong,
    msg_fin,
    msg_fin_ack,
    msg_dummy,
};

struct nanoping_send_request {
    enum nanoping_msg_type type;
    uint64_t seq;
    struct sockaddr_in remaddr;
};

struct nanoping_send_dummies_request {
    struct sockaddr_in remaddr;
    int nmsg;
};

struct nanoping_receive_result {
    enum nanoping_msg_type type;
    uint64_t seq;
    struct sockaddr_in remaddr;
};

// The actual header sent in the packet
struct nanoping_msg {
    uint64_t seq;
    uint8_t type;
    uint8_t reserved[7];
};

_Static_assert(sizeof(struct nanoping_msg) == 16,
               "Unexpected size of struct nanoping_msg - check for padding");

struct nanoping_instance *nanoping_init(char *interface, char *port,
    bool server, bool emulation, int timeout, int busy_poll,
    const char *log_path, bool log_pktdir);
int nanoping_wait_for_receive(struct nanoping_instance *ins);
ssize_t nanoping_receive_one(struct nanoping_instance *ins,
			     struct nanoping_receive_result *result,
			     void *payload_buf, size_t *payload_len);
ssize_t nanoping_send_one(struct nanoping_instance *ins,
			  struct nanoping_send_request *request,
			  void *payload_buf, size_t payload_len);
int nanoping_send_dummies(struct nanoping_instance *ins,
    struct nanoping_send_dummies_request *request);
int nanoping_txs_one(struct nanoping_instance *ins);
void nanoping_reset_state(struct nanoping_instance *ins);
void nanoping_finish(struct nanoping_instance *ins);

#endif
