// for sendmmsg
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <assert.h>
#include <stdarg.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <linux/errqueue.h>
#include "nanoping.h"

#define TXS_REC_LEN 6
#define PURGE_SEQ_AGE 100
struct nanoping_msg_txs_record {
    uint64_t seq;
    struct nanoping_timeval txs;
};

struct nanoping_msg {
    uint64_t seq;
    uint8_t type;
    struct nanoping_timeval rxs;
    uint8_t txs_rec_len;
    struct nanoping_msg_txs_record txs_rec[TXS_REC_LEN];
} __attribute__((__packed__));

struct nanoping_rx_record {
    uint64_t seq;
    struct timespec stamp;
    struct nanoping_timeval rem_rxs;
    uint8_t txs_rec_len;
    uint8_t rcvd_txs_rec;
    enum nanoping_receive_error err;
    TAILQ_ENTRY(nanoping_rx_record) entries;
};

struct nanoping_txs_record {
    uint64_t seq;
    struct timespec stamp;
    TAILQ_ENTRY(nanoping_txs_record) entries;
};

struct nanoping_emul_txs {
    uint64_t seq;
    struct timespec stamp;
} __attribute__((__packed__));

static int get_if_address(int fd, char *ifname, struct in_addr *addr)
{
    int res;
    struct ifreq ifr;
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ);
    ifr.ifr_addr.sa_family = AF_INET;
    if ((res = ioctl(fd, SIOCGIFADDR, &ifr)) < 0) {
        perror("SIOCGIFADDR");
        return res;
    }
    *addr = sin->sin_addr;
    return 0;
}

static int enable_hw_timestamp(int fd, char *ifname)
{
    int res;
    struct hwtstamp_config config = {.flags = 0, .tx_type = HWTSTAMP_TX_ON};
    struct ifreq ifr;
    unsigned int opt;
    int enabled = 1;

    config.rx_filter = HWTSTAMP_FILTER_ALL;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ);
    ifr.ifr_addr.sa_family = AF_INET;
    ifr.ifr_data = (void *)&config;
    if ((res = ioctl(fd, SIOCSHWTSTAMP, &ifr)) < 0) {
        perror("SIOCSHWTSTAMP");
        return res;
    }
    if (!config.tx_type) {
        fprintf(stderr, "HW TX timestamp doesn't supported on this NIC\n");
        return -1;
    }
    if (!config.rx_filter) {
        fprintf(stderr, "HW RX timestamp doesn't supported on this NIC\n");
        return -1;
    }
    opt = SOF_TIMESTAMPING_RX_HARDWARE |
        SOF_TIMESTAMPING_TX_HARDWARE |
        SOF_TIMESTAMPING_RAW_HARDWARE |
        SOF_TIMESTAMPING_OPT_CMSG;
    if ((res = setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, (char *)&opt,
                    sizeof(opt))) < 0) {
        perror("SO_TIMESTAMPING");
        return res;
    }
    if ((res = setsockopt(fd, SOL_SOCKET, SO_SELECT_ERR_QUEUE, &enabled,
                    sizeof(enabled))) < 0) {
        perror("SO_SELECT_ERR_QUEUE");
        return res;
    }
    if ((res = setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &enabled, 
                    sizeof(enabled))) < 0) {
        perror("IP_PKTINFO");
        return res;
    }

    return 0;
}

static inline ssize_t send_pkt_common(struct nanoping_instance *ins,
        struct sockaddr_in *remaddr, struct iovec *iov, uint64_t seq)
{
    struct msghdr m = {0};
    ssize_t siz;
    int res;

    m.msg_name = remaddr;
    m.msg_namelen = sizeof(struct sockaddr_in);
    m.msg_iov = iov;
    m.msg_iovlen = 1;

    errno  = 0;
    if ((siz = sendmsg(ins->fd, &m, 0)) <= 0) {
        if (errno == EAGAIN) {
            fprintf(stderr, "sendmsg: Request timed out.\n");
            return siz;
        }
        perror("sendmsg");
        return siz;
    }

    if (ins->emulation) {
        struct nanoping_emul_txs etxs;
        struct iovec iov2 = {&etxs, sizeof(etxs)};
        struct msghdr m2 = {0};
        struct timespec st;
        ssize_t esiz;

        if ((res = clock_gettime(CLOCK_REALTIME, &st))) {
            perror("clock_gettime");
            return res;
        }

        etxs.seq = seq;
        etxs.stamp = st;
        m2.msg_iov = &iov2;
        m2.msg_iovlen = 1;

        if ((esiz = sendmsg(ins->emul_fds[0], &m2, 0)) < 0) {
            perror("sendmsg");
            return siz;
        }
        assert(esiz == sizeof(etxs));
    }
    return siz;
}

static inline void init_nanoping_msg(struct nanoping_msg *msg, uint64_t seq,
        enum nanoping_msg_type type, struct timespec *prev_rxs,
        struct nanoping_msg_txs_record *txs_rec, int txs_rec_len)
{
    memset(msg, 0, sizeof(*msg));

    msg->seq = seq;
    msg->type = type;
    msg->rxs.tv_sec = prev_rxs->tv_sec;
    msg->rxs.tv_nsec = prev_rxs->tv_nsec;
    msg->txs_rec_len = TXS_REC_LEN;

    if (!txs_rec || txs_rec_len < 1)
        return;

    for (int i = 0; i < txs_rec_len; i++) {
        msg->txs_rec[i] = txs_rec[i];
    }
}

static inline ssize_t send_pkt_msg(struct nanoping_instance *ins, struct sockaddr_in *remaddr,
        uint64_t seq, struct timespec *prev_rxs,
        struct nanoping_msg_txs_record *txs_rec, int txs_rec_len,
        enum nanoping_msg_type type)
{
    char buf[MAX_PAD_BYTES + sizeof(struct nanoping_msg)] = {0};
    struct nanoping_msg *msg = (struct nanoping_msg *)&buf;
    struct iovec iov = {msg, sizeof(*msg) + ins->pad_bytes};

    init_nanoping_msg(msg, seq, type, prev_rxs, txs_rec, txs_rec_len);

    return send_pkt_common(ins, remaddr, &iov, seq);
}

static ssize_t send_pkt(struct nanoping_instance *ins, struct sockaddr_in *remaddr,
        uint64_t seq, struct timespec *prev_rxs,
        struct nanoping_msg_txs_record *txs_rec, int txs_rec_len,
        enum nanoping_msg_type type)
{
    return send_pkt_msg(ins, remaddr, seq, prev_rxs, txs_rec, txs_rec_len, type);
}

static int parse_control_msg(struct msghdr *m, struct timespec *stamp,
        int *stamp_found)
{
    struct cmsghdr *cm;
    struct timespec *received_stamp;
    struct sock_extended_err *exterr;

    *stamp_found = 0;
    for (cm = CMSG_FIRSTHDR(m); cm; cm = CMSG_NXTHDR(m, cm)) {
        switch (cm->cmsg_level) {
        case SOL_SOCKET:
            switch (cm->cmsg_type) {
            case SO_TIMESTAMPING:
                received_stamp = (struct timespec *)CMSG_DATA(cm);
                *stamp = received_stamp[2];
                *stamp_found = 1;
                break;
            default:
                fprintf(stderr, "Unexpected cmsg level:SOL_SOCKET type:%d\n",
                        cm->cmsg_type);
                return -1;
            }
            break;
        case IPPROTO_IP:
            switch (cm->cmsg_type) {
            case IP_RECVERR:
                exterr = (struct sock_extended_err *)CMSG_DATA(cm);
                if (!(exterr->ee_errno == ENOMSG &&
                        exterr->ee_origin == SO_EE_ORIGIN_TIMESTAMPING))
                    fprintf(stderr, "Unexcepted recverr errno '%s' origin %d\n",
                            strerror(exterr->ee_errno), exterr->ee_origin);
                break;
            case IP_PKTINFO:
                break;
            default:
                fprintf(stderr, "Unexpected cmsg level:IPPROTO_IP type:%d\n",
                        cm->cmsg_type);
                return -1;
            }
            break;
        default:
                fprintf(stderr, "Unexpected cmsg level:%d type:%d\n",
                        cm->cmsg_level, cm->cmsg_type);
                return -1;
        }
    }
    return 0;
}

static ssize_t receive_pkt_common(struct nanoping_instance *ins,
        struct iovec *iov, struct timespec *stamp,
        struct sockaddr_in *remaddr, enum nanoping_receive_error *err)
{
    struct msghdr m = {0};
    char ctrlbuf[1024];
    ssize_t siz;
    int res;
    int stamp_found;

    m.msg_iov = iov;
    m.msg_iovlen = 1;
    m.msg_name = remaddr;
    m.msg_namelen = sizeof(struct sockaddr_in);
    m.msg_control = ctrlbuf;
    m.msg_controllen = sizeof(ctrlbuf);

    errno  = 0;
    if ((siz = recvmsg(ins->fd, &m, 0)) < 0) {
        if (errno == EAGAIN) {
            fprintf(stderr, "recvmsg: Request timed out.\n");
            return -EAGAIN;
        }
        perror("recvmsg");
        return siz;
    }

    ins->pkt_received++;
    if (ins->emulation) {
        if ((res = clock_gettime(CLOCK_REALTIME, stamp))) {
            perror("clock_gettime");
            return res;
        }
        stamp_found = 1;
    }else{
        /* on RX side, control message comes with data */
        if ((res = parse_control_msg(&m, stamp, &stamp_found)) < 0)
            return res;
    }

    if (stamp_found) {
        ins->rxs_collected++;
    }else{
        *err |= rxerr_rxs_failed;
    }

    return siz;
}

static ssize_t receive_pkt_msg(struct nanoping_instance *ins,
        struct nanoping_msg *msg, struct timespec *stamp,
        struct sockaddr_in *remaddr, enum nanoping_receive_error *err)
{
    struct iovec iov = {msg, sizeof(*msg)};

    ssize_t siz = receive_pkt_common(ins, &iov, stamp, remaddr, err);
    if (siz < (ssize_t)sizeof(*msg))
	    return siz < 0 ? siz : -ENOMSG;

    if (msg->seq > 1) {
        if (msg->rxs.tv_sec == 0 && msg->rxs.tv_nsec == 0)
            *err |= rxerr_rem_rxs_failed;
        else
            ins->rem_rxs_collected++;
    }
    for (int i = 0; i < msg->txs_rec_len; i++) {
        if (msg->txs_rec[i].seq)
            ins->rem_txs_collected++;
    }

    return siz;
}

struct nanoping_instance *nanoping_init(char *interface, char *port,
        bool server, bool emulation, int timeout, int pad_bytes, int busy_poll)
{
    struct nanoping_instance *ins =
        (struct nanoping_instance *)calloc(1, sizeof(*ins));
    int res;

    assert(ins);

    if ((ins->fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket");
        return NULL;
    }

    if ((ins->nots_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket");
        return NULL;
    }

    if ((res = setsockopt(ins->fd, SOL_SOCKET, SO_BINDTODEVICE, interface,
                    strlen(interface)+1)) < 0) {
        perror("SO_BINDTODEVICE");
        return NULL;
    }

    if ((res = setsockopt(ins->nots_fd, SOL_SOCKET, SO_BINDTODEVICE, interface,
                    strlen(interface)+1)) < 0) {
        perror("SO_BINDTODEVICE");
        return NULL;
    }

    if (timeout) {
        struct timeval timeo = { timeout / 1000000, timeout % 1000000 };
        if ((res = setsockopt(ins->fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeo,
                        sizeof(timeo))) < 0) {
            perror("SO_RCVTIMEO");
            return NULL;
        }

        if ((res = setsockopt(ins->fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeo,
                        sizeof(timeo))) < 0) {
            perror("SO_SNDTIMEO");
            return NULL;
        }

        if ((res = setsockopt(ins->nots_fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeo,
                        sizeof(timeo))) < 0) {
            perror("SO_RCVTIMEO");
            return NULL;
        }

        if ((res = setsockopt(ins->nots_fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeo,
                        sizeof(timeo))) < 0) {
            perror("SO_SNDTIMEO");
            return NULL;
        }
    }

    ins->myaddr.sin_family = AF_INET;
    ins->myaddr.sin_port = htons(atoi(port));
    if (get_if_address(ins->fd, interface, &ins->myaddr.sin_addr) < 0) {
        perror("get_if_address");
        return NULL;
    }

    ins->server = server;
    if (server) {
        if (bind(ins->fd, (struct sockaddr *)&ins->myaddr, sizeof(ins->myaddr)) < 0) {
            perror("bind");
            return NULL;
        }
    }

    if (emulation) {
        ins->emulation = true;
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, ins->emul_fds) < 0) {
            perror("socketpair");
            return NULL;
        }
    }else{
        ins->emulation = false;
        if (enable_hw_timestamp(ins->fd, interface) < 0)
            return NULL;
    }
    ins->pad_bytes = pad_bytes;

    if (busy_poll) {
        if ((res = setsockopt(ins->fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll,
                        sizeof(busy_poll))) < 0) {
            perror("SO_BUSY_POLL");
            return NULL;
        }
    }
    TAILQ_INIT(&ins->rx_head);
    TAILQ_INIT(&ins->rx4proc_head);
    TAILQ_INIT(&ins->rx4tx_head);
    TAILQ_INIT(&ins->txs_head);
    TAILQ_INIT(&ins->txs4proc_head);
    TAILQ_INIT(&ins->rem_txs_head);
    pthread_mutex_init(&ins->rx_lock, NULL);
    pthread_mutex_init(&ins->rx4proc_lock, NULL);
    pthread_mutex_init(&ins->rx4tx_lock, NULL);
    pthread_mutex_init(&ins->txs_lock, NULL);
    pthread_mutex_init(&ins->txs4proc_lock, NULL);
    pthread_mutex_init(&ins->rem_txs_lock, NULL);
    return ins;
}

static void append_rx_pong_rec(struct nanoping_instance *ins, struct nanoping_msg *msg, struct timespec *stamp, enum nanoping_receive_error err)
{
    struct nanoping_rx_record *rec, *rec2;

    rec = malloc(sizeof(*rec));
    assert(rec);
    rec->seq = msg->seq;
    rec->stamp = *stamp;
    rec->rem_rxs = msg->rxs;
    rec->txs_rec_len = msg->txs_rec_len;
    rec->err = err;
    for (int i = 0; i < msg->txs_rec_len; i++) {
        if (!msg->txs_rec[i].seq) {
            rec->rcvd_txs_rec = i;
            break;
        }
    }
    pthread_mutex_lock(&ins->rx_lock);
    TAILQ_INSERT_TAIL(&ins->rx_head, rec, entries);
    pthread_mutex_unlock(&ins->rx_lock);

    if (msg->seq <= 1)
        return;

    rec2 = malloc(sizeof(*rec2));
    assert(rec2);
    *rec2 = *rec;
    pthread_mutex_lock(&ins->rx4proc_lock);
    TAILQ_INSERT_TAIL(&ins->rx4proc_head, rec2, entries);
    pthread_mutex_unlock(&ins->rx4proc_lock);

    for (int i = 0; i < msg->txs_rec_len; i++) {
        if (!msg->txs_rec[i].seq)
            break;
        struct nanoping_txs_record *txs_rec = calloc(1, sizeof(*txs_rec));
        assert(txs_rec);

        txs_rec->seq = msg->txs_rec[i].seq;
        txs_rec->stamp.tv_sec = msg->txs_rec[i].txs.tv_sec;
        txs_rec->stamp.tv_nsec = msg->txs_rec[i].txs.tv_nsec;
        pthread_mutex_lock(&ins->rem_txs_lock);
        TAILQ_INSERT_TAIL(&ins->rem_txs_head, txs_rec, entries);
        pthread_mutex_unlock(&ins->rem_txs_lock);
    }
}

static void append_rx_ping_rec(struct nanoping_instance *ins, struct nanoping_msg *msg, struct timespec *stamp, enum nanoping_receive_error err)
{
    struct nanoping_rx_record *rec;

    rec = calloc(1, sizeof(*rec));
    assert(rec);
    rec->seq = msg->seq;
    rec->rem_rxs = msg->rxs;
    rec->stamp = *stamp;

    pthread_mutex_lock(&ins->rx4tx_lock);
    TAILQ_INSERT_TAIL(&ins->rx4tx_head, rec, entries);
    pthread_mutex_unlock(&ins->rx4tx_lock);
}

/* return -1 when error
 * return 0 when no message
 * return 1 when message available
 */
int nanoping_process_one(struct nanoping_instance *ins,
    struct nanoping_process_result *result)
{
    struct nanoping_rx_record *cur, *first = NULL;
    struct nanoping_rx_record *rx_rec;
    struct nanoping_txs_record *txs_rec;
    struct nanoping_txs_record *rem_txs_rec;

    assert(ins && result);
    result->error = 0;
    for (;;) {
        bool rem_txs_found = false, rx_found = false, txs_found = false;

        pthread_mutex_lock(&ins->rx4proc_lock);
        cur = TAILQ_FIRST(&ins->rx4proc_head);
        if (!cur || cur == first) {
            pthread_mutex_unlock(&ins->rx4proc_lock);
            return 0;
        }
        TAILQ_REMOVE(&ins->rx4proc_head, cur, entries);
        pthread_mutex_unlock(&ins->rx4proc_lock);
        if (!first)
            first = cur;

        if (cur->err) {
            result->seq = cur->seq;
            result->error = cur->err;
            free(cur);
            return -1;
        }

        pthread_mutex_lock(&ins->rem_txs_lock);
        TAILQ_FOREACH(rem_txs_rec, &ins->rem_txs_head, entries) {
            if (rem_txs_rec->seq == cur->seq - 1) {
                TAILQ_REMOVE(&ins->rem_txs_head, rem_txs_rec, entries);
                rem_txs_found = true;
                break;
            }
        }
        pthread_mutex_unlock(&ins->rem_txs_lock);
        if (!rem_txs_found)
            goto not_ready;

        pthread_mutex_lock(&ins->rx_lock);
        TAILQ_FOREACH(rx_rec, &ins->rx_head, entries) {
            if (rx_rec->seq == cur->seq - 1) {
                TAILQ_REMOVE(&ins->rx_head, rx_rec, entries);
                rx_found = true;
                ins->rx_prev_rxs_found++;
                break;
            }
        }
        pthread_mutex_unlock(&ins->rx_lock);
        if (!rx_found)
            goto not_ready;

        pthread_mutex_lock(&ins->txs4proc_lock);
        TAILQ_FOREACH(txs_rec, &ins->txs4proc_head, entries) {
            if (txs_rec->seq == cur->seq - 1) {
                TAILQ_REMOVE(&ins->txs4proc_head, txs_rec, entries);
                txs_found = true;
                ins->rx_prev_txs_found++;
                break;
            }
        }
        pthread_mutex_unlock(&ins->txs4proc_lock);
        if (!txs_found)
            goto not_ready;

        result->seq = cur->seq;
        result->t0.tv_sec = txs_rec->stamp.tv_sec;
        result->t0.tv_nsec = txs_rec->stamp.tv_nsec;
        result->t1 = rx_rec->rem_rxs;
        result->t2.tv_sec = rem_txs_rec->stamp.tv_sec;
        result->t2.tv_nsec = rem_txs_rec->stamp.tv_nsec;
        result->t3.tv_sec = rx_rec->stamp.tv_sec;
        result->t3.tv_nsec = rx_rec->stamp.tv_nsec;

        free(cur);
        free(rem_txs_rec);
        free(rx_rec);
        free(txs_rec);
        return 1;
    not_ready:
        if (cur->seq + PURGE_SEQ_AGE < ins->sent_seq) {
            if (!rem_txs_found)
                result->error |= rxerr_rem_txs_failed;
            if (!rx_found)
                result->error |= rxerr_rx_dropped;
            if (!txs_found)
                result->error |= rxerr_tx_dropped;
            free(cur);
            return -1;
        }
        pthread_mutex_lock(&ins->rx4proc_lock);
        TAILQ_INSERT_TAIL(&ins->rx4proc_head, cur, entries);
        pthread_mutex_unlock(&ins->rx4proc_lock);
    }
}

int nanoping_wait_for_receive(struct nanoping_instance *ins)
{
    fd_set readfds;
    int res;

retry:
    FD_ZERO(&readfds);
    FD_SET(ins->fd, &readfds);
    if ((res = select(ins->fd + 1, &readfds, 0, NULL, NULL)) < 0) {
        perror("select");
        return res;
    } else if (res == 0)
        goto retry;
    return res;
}

ssize_t nanoping_receive_one(struct nanoping_instance *ins,
    struct nanoping_receive_result *result)
{
    struct timespec stamp;
    struct nanoping_msg msg;
    enum nanoping_receive_error err = 0;
    ssize_t siz;

    assert(ins && result);

    siz = receive_pkt_msg(ins, &msg, &stamp, &result->remaddr, &err);

    if (siz < 0)
        return siz;

    if (msg.type == msg_ping)
        append_rx_ping_rec(ins, &msg, &stamp, err);

    if (msg.type == msg_pong)
        append_rx_pong_rec(ins, &msg, &stamp, err);

    result->seq = msg.seq;
    result->type = msg.type;

    return siz;
}

ssize_t nanoping_send_one(struct nanoping_instance *ins,
    struct nanoping_send_request *request)
{
    static struct timespec zero_stamp = {0};
    static struct timespec prev_stamp = {0};
    struct timespec *rxs = &zero_stamp;
    struct nanoping_msg_txs_record txs_rec[TXS_REC_LEN] = {{0}};
    int i = 0;
    ssize_t siz;

    assert(ins && request);
    if (request->type == msg_pong) {
        {
            struct nanoping_txs_record *rec;
            struct nanoping_txs_record *recs[TXS_REC_LEN] = {NULL};

            if (!pthread_mutex_trylock(&ins->txs_lock)) {
                ins->sent_seq = request->seq;
                TAILQ_FOREACH(rec, &ins->txs_head, entries) {
                    recs[i++] = rec;
                    TAILQ_REMOVE(&ins->txs_head, rec, entries);
                    if (i >= TXS_REC_LEN)
                        break;
                }
                pthread_mutex_unlock(&ins->txs_lock);
            }
            for (int j = 0; j < i; j++) {
                ins->tx_prev_rxs_found++;
                txs_rec[j].seq = recs[j]->seq;
                txs_rec[j].txs.tv_sec = recs[j]->stamp.tv_sec;
                txs_rec[j].txs.tv_nsec = recs[j]->stamp.tv_nsec;
                free(recs[j]);
            }
        }
        {
            struct nanoping_rx_record *rec;
            bool found = false;

            pthread_mutex_lock(&ins->rx4tx_lock);
            TAILQ_FOREACH(rec, &ins->rx4tx_head, entries) {
                if (rec->seq == request->seq) {
                    prev_stamp = rec->stamp;
                    rxs = &prev_stamp;
                    TAILQ_REMOVE(&ins->rx4tx_head, rec, entries);
                    found = true;
                    break;
                }
            }
            pthread_mutex_unlock(&ins->rx4tx_lock);
            if (found) {
                ins->tx_prev_rxs_found++;
                free(rec);
            }
        }
    }
    if ((siz = send_pkt(ins, &request->remaddr, request->seq, rxs, txs_rec, i, request->type)) < 0)
        return siz;
    ins->pkt_transmitted++;
    return siz;
}

static int send_dummies_common(struct nanoping_instance *ins, struct sockaddr_in *remaddr, int nmsg, struct iovec *iovs)
{
    struct mmsghdr mmsgs[nmsg];
    int res;

    memset(mmsgs, 0, sizeof(mmsgs));
    for (int i = 0; i < nmsg; i++) {
        mmsgs[i].msg_hdr.msg_name = remaddr;
        mmsgs[i].msg_hdr.msg_namelen = sizeof(struct sockaddr_in);
        mmsgs[i].msg_hdr.msg_iov = &iovs[i];
        mmsgs[i].msg_hdr.msg_iovlen = 1;
    }

    res = sendmmsg(ins->nots_fd, mmsgs, nmsg, 0);
    if (!res) {
        fprintf(stderr, "zero dummy packets sent\n");
    } else if (res < 0) {
        perror("sendmmsg");
    }
    ins->pkt_transmitted += res;
    return res;
}

static int send_dummies_msg(struct nanoping_instance *ins, struct sockaddr_in *remaddr, int nmsg)
{
    struct nanoping_msg msgs[nmsg];
    struct iovec iovs[nmsg];

    memset(msgs, 0, sizeof(msgs));
    memset(iovs, 0, sizeof(iovs));
    for (int i = 0; i < nmsg; i++) {
        msgs[i].seq = UINT64_MAX;
        msgs[i].type = msg_dummy;
        iovs[i].iov_base = &msgs[i];
        iovs[i].iov_len = sizeof(struct nanoping_msg);
    }
    return send_dummies_common(ins, remaddr, nmsg, iovs);
}

int nanoping_send_dummies(struct nanoping_instance *ins, struct nanoping_send_dummies_request *request)
{
    assert(ins && request);

    return send_dummies_msg(ins, &request->remaddr, request->nmsg);
}

static int append_txs_rec(struct nanoping_instance *ins, uint64_t seq, struct timespec stamp)
{
    int res;
    struct nanoping_txs_record *rec = calloc(1, sizeof(*rec));
    struct nanoping_txs_record *rec2;

    if (ins->server) {
    	rec2 = calloc(1, sizeof(*rec2));
    	assert(rec2);
    }
    assert(rec);
    ins->txs_collected++;
    rec->seq = seq;
    rec->stamp = stamp;
    if (ins->server) {
    	*rec2 = *rec;
    }
    if ((res = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL)) < 0) {
        perror("pthread_setcancelstate");
        return res;
    }
    if (ins->server) {
        pthread_mutex_lock(&ins->txs_lock);
	if (ins->sent_seq > seq)
	    ins->tx_prev_txs_too_late++;
        TAILQ_INSERT_TAIL(&ins->txs_head, rec2, entries);
        pthread_mutex_unlock(&ins->txs_lock);
    }
    pthread_mutex_lock(&ins->txs4proc_lock);
    TAILQ_INSERT_TAIL(&ins->txs4proc_head, rec, entries);
    pthread_mutex_unlock(&ins->txs4proc_lock);
    if ((res = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL)) < 0) {
        perror("pthread_setcancelstate");
        return res;
    }
    return 0;
}

int nanoping_txs_one(struct nanoping_instance *ins)
{
    fd_set exceptfds;
    struct sockaddr_in remaddr;
    struct msghdr m = {0};
    char pktbuf[2048];
    char ctrlbuf[1024];
    struct iovec iov = {pktbuf, sizeof(pktbuf)};
    struct nanoping_msg *msg;
    int res;
    ssize_t siz, headsiz;
    int stamp_found = 0;
    struct timespec stamp;

    if (ins->emulation) {
        struct nanoping_emul_txs etxs;
        struct iovec iov2 = {&etxs, sizeof(etxs)};
        struct msghdr m2 = {0};
        m2.msg_iov = &iov2;
        m2.msg_iovlen = 1;

        errno = 0;
        if ((siz = recvmsg(ins->emul_fds[1], &m2, 0)) < 0) {
            perror("recvmsg");
            return siz;
        }
        assert(siz == sizeof(etxs));
        return append_txs_rec(ins, etxs.seq, etxs.stamp);
    }
    FD_ZERO(&exceptfds);
    FD_SET(ins->fd, &exceptfds);
    if ((res = select(ins->fd + 1, NULL, NULL, &exceptfds, NULL)) == 0) {
        fprintf(stderr, "Timeout to receive tx timestamp\n");
        return -1;
    } else if (res < 0) {
        perror("select");
        return res;
    } else if (!FD_ISSET(ins->fd, &exceptfds)) {
        fprintf(stderr, "No message available on error queue\n");
        return -1;
    }

    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_name = (void *)&remaddr;
    m.msg_namelen = sizeof(remaddr);
    m.msg_control = ctrlbuf;
    m.msg_controllen = sizeof(ctrlbuf);

    errno  = 0;
    if ((siz = recvmsg(ins->fd, &m, MSG_ERRQUEUE | MSG_DONTWAIT)) < 0) {
        if (errno == EAGAIN) {
            fprintf(stderr, "recvmsg(errqueue): Request timed out.\n");
            return siz;
        }
        perror("recvmsg(errqueue)");
        return siz;
    }
    if (siz < sizeof(*msg) + ins->pad_bytes) {
        fprintf(stderr, "Invalid packet size on txs callback\n");
        return -1;
    }
    headsiz = siz - (sizeof(*msg) + ins->pad_bytes);
    msg = (struct nanoping_msg *)(((char *)pktbuf) + headsiz);

    if ((res = parse_control_msg(&m, &stamp, &stamp_found)) < 0)
        return res;

    if (stamp_found) {
        return append_txs_rec(ins, msg->seq, stamp);
    }

    return 0;
}

void nanoping_reset_state(struct nanoping_instance *ins)
{
    struct nanoping_rx_record *rx_rec;
    struct nanoping_txs_record *tx_rec;

    ins->pkt_received = 0;
    ins->pkt_transmitted = 0;
    ins->rxs_collected = 0;
    ins->txs_collected = 0;
    ins->rem_rxs_collected = 0;
    ins->rem_txs_collected = 0;
    ins->rx_prev_rxs_found = 0;
    ins->rx_prev_txs_found = 0;
    ins->tx_prev_rxs_found = 0;
    ins->tx_prev_txs_found = 0;
    ins->tx_prev_txs_too_late = 0;

    pthread_mutex_lock(&ins->rx_lock);
    while (!TAILQ_EMPTY(&ins->rx_head)) {
        rx_rec = TAILQ_FIRST(&ins->rx_head);
        TAILQ_REMOVE(&ins->rx_head, rx_rec, entries);
        free(rx_rec);
    }
    pthread_mutex_unlock(&ins->rx_lock);

    pthread_mutex_lock(&ins->rx4proc_lock);
    while (!TAILQ_EMPTY(&ins->rx4proc_head)) {
        rx_rec = TAILQ_FIRST(&ins->rx4proc_head);
        TAILQ_REMOVE(&ins->rx4proc_head, rx_rec, entries);
        free(rx_rec);
    }
    pthread_mutex_unlock(&ins->rx4proc_lock);


    pthread_mutex_lock(&ins->rx4tx_lock);
    while (!TAILQ_EMPTY(&ins->rx4tx_head)) {
        rx_rec = TAILQ_FIRST(&ins->rx4tx_head);
        TAILQ_REMOVE(&ins->rx4tx_head, rx_rec, entries);
        free(rx_rec);
    }
    pthread_mutex_unlock(&ins->rx4tx_lock);


    pthread_mutex_lock(&ins->txs_lock);
    while (!TAILQ_EMPTY(&ins->txs_head)) {
        tx_rec = TAILQ_FIRST(&ins->txs_head);
        TAILQ_REMOVE(&ins->txs_head, tx_rec, entries);
        free(tx_rec);
    }
    pthread_mutex_unlock(&ins->txs_lock);

    pthread_mutex_lock(&ins->txs4proc_lock);
    while (!TAILQ_EMPTY(&ins->txs4proc_head)) {
        tx_rec = TAILQ_FIRST(&ins->txs4proc_head);
        TAILQ_REMOVE(&ins->txs4proc_head, tx_rec, entries);
        free(tx_rec);
    }
    pthread_mutex_unlock(&ins->txs4proc_lock);

    pthread_mutex_lock(&ins->rem_txs_lock);
    while (!TAILQ_EMPTY(&ins->rem_txs_head)) {
        tx_rec = TAILQ_FIRST(&ins->rem_txs_head);
        TAILQ_REMOVE(&ins->rem_txs_head, tx_rec, entries);
        free(tx_rec);
    }
    pthread_mutex_unlock(&ins->rem_txs_lock);
}

void nanoping_finish(struct nanoping_instance *ins)
{
    assert(ins);

    if (ins->fd) {
        close(ins->fd);
    }

    if (ins->emulation) {
        close(ins->emul_fds[0]);
        close(ins->emul_fds[1]);
    }
}

