#ifndef NANOPING_H
#define NANOPING_H

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>

#define NS_PER_S 1000000000
#define NS_PER_US 1000

#define MAX_PAD_BYTES 2048

enum timestamp_index {
    TSTAMP_IDX_SENDPING = 0,
    TSTAMP_IDX_RECVPING = 1,
    TSTAMP_IDX_SENDPONG = 2,
    TSTAMP_IDX_RECVPONG = 3,
};

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
    int pad_bytes;
    FILE *log_stream;
    unsigned long pkt_received;
    unsigned long pkt_transmitted;
    unsigned long rxs_collected;
    unsigned long txs_collected;
    uint64_t sent_seq;
};

enum nanoping_msg_type {
    msg_none = 0,
    msg_syn,
    msg_syn_ack,
    msg_syn_rst,
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

struct nanoping_instance *nanoping_init(char *interface, char *port,
    bool server, bool emulation, int timeout, int pad_bytes, int busy_poll,
    const char *log_path);
int nanoping_wait_for_receive(struct nanoping_instance *ins);
ssize_t nanoping_receive_one(struct nanoping_instance *ins,
    struct nanoping_receive_result *result);
ssize_t nanoping_send_one(struct nanoping_instance *ins,
    struct nanoping_send_request *request);
int nanoping_send_dummies(struct nanoping_instance *ins,
    struct nanoping_send_dummies_request *request);
int nanoping_txs_one(struct nanoping_instance *ins);
void nanoping_reset_state(struct nanoping_instance *ins);
void nanoping_finish(struct nanoping_instance *ins);

#endif
