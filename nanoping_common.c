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

struct nanoping_msg {
    uint64_t seq;
    uint8_t type;
    uint8_t reserved[7];
};

_Static_assert(sizeof(struct nanoping_msg) == 16,
               "Unexpected size of struct nanoping_msg - check for padding");

struct nanoping_emul_txs {
    uint64_t seq;
    struct timespec stamp;
};

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
                                     enum nanoping_msg_type type)
{
    memset(msg, 0, sizeof(*msg));

    msg->seq = seq;
    msg->type = type;
}

static inline ssize_t send_pkt_msg(struct nanoping_instance *ins,
                                   struct sockaddr_in *remaddr, uint64_t seq,
                                   enum nanoping_msg_type type)
{
    char buf[MAX_PAD_BYTES + sizeof(struct nanoping_msg)] = {0};
    struct nanoping_msg *msg = (struct nanoping_msg *)&buf;
    struct iovec iov = {msg, sizeof(*msg) + ins->pad_bytes};

    init_nanoping_msg(msg, seq, type);

    return send_pkt_common(ins, remaddr, &iov, seq);
}

static ssize_t send_pkt(struct nanoping_instance *ins,
                        struct sockaddr_in *remaddr, uint64_t seq,
                        enum nanoping_msg_type type)
{
    return send_pkt_msg(ins, remaddr, seq, type);
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
        struct sockaddr_in *remaddr)
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

    if (stamp_found)
        ins->rxs_collected++;

    return siz;
}

static ssize_t receive_pkt_msg(struct nanoping_instance *ins,
        struct nanoping_msg *msg, struct timespec *stamp,
        struct sockaddr_in *remaddr)
{
    struct iovec iov = {msg, sizeof(*msg)};

    ssize_t siz = receive_pkt_common(ins, &iov, stamp, remaddr);
    if (siz < (ssize_t)sizeof(*msg))
	    return siz < 0 ? siz : -ENOMSG;

    return siz;
}

struct nanoping_instance *nanoping_init(char *interface, char *port,
    bool server, bool emulation, int timeout, int pad_bytes, int busy_poll,
    const char *log_path)
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

    if (log_path) {
        ins->log_stream = fopen(log_path, "a");
        if (!ins->log_stream) {
            perror("fopen(log_path)");
            return NULL;
        }

        fprintf(ins->log_stream, "seq,timestamp-idx,timestamp\n");
    } else {
        ins->log_stream = NULL;
    }

    return ins;
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

static void log_pkt_tstamp(const struct nanoping_instance *ins, uint64_t seq,
                   const struct timespec *tstamp,
                   enum timestamp_index tstamp_idx)
{
    if (!ins || !ins->log_stream || seq == 0)
        return;

    fprintf(ins->log_stream, "%lu,t%d,%lu.%09lu\n", seq, tstamp_idx,
	    tstamp->tv_sec, tstamp->tv_nsec);
}

ssize_t nanoping_receive_one(struct nanoping_instance *ins,
    struct nanoping_receive_result *result)
{
    struct timespec stamp;
    struct nanoping_msg msg;
    ssize_t siz;

    assert(ins && result);

    siz = receive_pkt_msg(ins, &msg, &stamp, &result->remaddr);

    if (siz < 0)
        return siz;

    if (msg.type == msg_ping)
        log_pkt_tstamp(ins, msg.seq, &stamp, TSTAMP_IDX_RECVPING);

    if (msg.type == msg_pong)
        log_pkt_tstamp(ins, msg.seq, &stamp, TSTAMP_IDX_RECVPONG);

    result->seq = msg.seq;
    result->type = msg.type;

    return siz;
}

ssize_t nanoping_send_one(struct nanoping_instance *ins,
    struct nanoping_send_request *request)
{
    ssize_t siz;

    assert(ins && request);
    if ((siz = send_pkt(ins, &request->remaddr, request->seq, request->type)) < 0)
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
        ins->txs_collected++;
        // Don't know packet type, so guessing type based on if server or not
        log_pkt_tstamp(ins, etxs.seq, &etxs.stamp,
                       ins->server ? TSTAMP_IDX_SENDPONG : TSTAMP_IDX_SENDPING);
        return 0;
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
        ins->txs_collected++;
        log_pkt_tstamp(ins, msg->seq, &stamp,
                       msg->type == msg_ping ? TSTAMP_IDX_SENDPING :
                                               TSTAMP_IDX_SENDPONG);
    }

    return 0;
}

void nanoping_reset_state(struct nanoping_instance *ins)
{
    ins->pkt_received = 0;
    ins->pkt_transmitted = 0;
    ins->rxs_collected = 0;
    ins->txs_collected = 0;
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

    if (ins->log_stream) {
        fclose(ins->log_stream);
        ins->log_stream = NULL;
    }
}

