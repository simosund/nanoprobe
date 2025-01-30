#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <netdb.h>
#include <pthread.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <limits.h>
#include <assert.h>
#include <stdatomic.h>
#include <errno.h>
#include <arpa/inet.h>
#include "nanoping.h"

// Some limits for acceptable values for server in reverse mode
#define REVERSE_DEFAULT_COUNT 10
#define REVERSE_MAX_COUNT 3600 // 1h
#define REVERSE_MAX_DELAY 1000000 // 1sec

enum timer_type {
    timer_sleep = 0,
    timer_busy,
    timer_invalid,
    timer_n_types = timer_invalid
};

enum nanoping_mode {
    mode_none = 0,
    mode_server,
    mode_client
};

struct nanoping_client_opts {
	uint32_t count;
	uint32_t delay;
	enum timer_type ttype;
	bool reverse;
};

static struct option longopts[] = {
    {"interface",  required_argument, NULL, 'i'},
    {"count",      required_argument, NULL, 'n'},
    {"delay",      required_argument, NULL, 'd'},
    {"port",       required_argument, NULL, 'p'},
    {"log",        required_argument, NULL, 'l'},
    {"emulation",  no_argument,       NULL, 'e'},
    {"timeout",    required_argument, NULL, 't'},
    {"busypoll",   required_argument, NULL, 'b'},
    {"dummy-pkt",  required_argument, NULL, 'x'},
    {"pad-bytes",  required_argument, NULL, 'B'},
    {"timer",      required_argument, NULL, 'T'},
    {"reverse",    no_argument,       NULL, 'R'},
    {"help",       no_argument,       NULL, 'h'},
    {0,            0,                 0,     0 }
};

static atomic_bool signal_initialized = ATOMIC_VAR_INIT(false);
static atomic_bool signal_handled = ATOMIC_VAR_INIT(false);
static _Atomic enum nanoping_msg_type state = ATOMIC_VAR_INIT(msg_none);
static pthread_mutex_t signal_lock;
static pthread_cond_t signal_cond;
static void usage(void)
{
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "  client: nanoping --client --interface [nic] --count [sec] --delay [usec] --port [port] --log [log] --emulation --timeout [usec] --busypoll [usec] --dummy-pkt [cnt] --reverse [host]\n");
    fprintf(stderr, "  server: nanoping --server --interface [nic] --port [port] --log [log] --emulation --timeout [usec] --busypoll [usec] --dummy-pkt [cnt]\n");
}

inline static double percent_ulong(unsigned long v1, unsigned long v2)
{
    return (((double)(v1 - v2)) * 100) / v1;
}

#define timevalprint_ns(tvp) \
    do { \
        if ((tvp)->tv_sec) \
            printf("%ld", (tvp)->tv_sec); \
        printf("%ld", (tvp)->tv_nsec); \
    } while (0)

#define timevalprint_s(tvp) \
    do { \
        if ((tvp)->tv_sec) \
            printf("%ld.", (tvp)->tv_sec); \
        else \
            printf("0."); \
        printf("%ld", (tvp)->tv_nsec); \
    } while (0)

static uint64_t timespec_to_ns(const struct timespec *ts)
{
    return (uint64_t)ts->tv_sec * NS_PER_S + ts->tv_nsec;
}

static uint64_t clock_gettime_ns(clockid_t clockid)
{
    struct timespec ts;
    if (clock_gettime(clockid, &ts) < 0)
        return 0;

    return timespec_to_ns(&ts);
}

static const char *timertype_to_str(enum timer_type ttype)
{
    switch (ttype) {
        case timer_sleep:
            return "sleep";
        case timer_busy:
            return "busy";
        default:
            return "invalid";
    }
}

static enum timer_type str_to_timertype(const char *str)
{
    enum timer_type tt;

    for (tt = 0; tt < timer_n_types; tt++) {
        if (strcmp(str, timertype_to_str(tt)) == 0)
            return tt;
    }

    return timer_invalid;
}

static void dump_statistics(struct nanoping_instance *ins, struct timespec *duration, bool client, size_t size)
{
    assert(ins);
    assert(duration);

    printf("--- nanoping statistics ---\n");
    printf("packet size %zd bytes\n",
            size);
    printf("%lu packets transmitted, %lu received, ",
            ins->pkt_transmitted,
            ins->pkt_received);
    if (client)
	    printf("%f%% packet loss, ",
            percent_ulong(ins->pkt_transmitted, ins->pkt_received));
    printf("time ");
    timevalprint_s(duration);
    printf(" s\n");
    printf("%lu RX stamp collected, %lu TX stamp collected\n",
            ins->rxs_collected,
            ins->txs_collected);
}

static void *process_client_receive_task(void *arg)
{
    struct nanoping_instance *ins = (struct nanoping_instance *)arg;
    struct nanoping_receive_result receive_result = {0};
    ssize_t siz;


    nanoping_wait_for_receive(ins);
    for (;;) {
        siz = nanoping_receive_one(ins, &receive_result, NULL, NULL);
        if (siz < 0)
            exit(EXIT_FAILURE);

        switch (receive_result.type) {
            case msg_syn_ack:
                if (atomic_load(&state) == msg_syn)
                    atomic_store(&state, msg_syn_ack);
                break;
            case msg_syn_rst:
                if (atomic_load(&state) != msg_syn)
                    break;
                atomic_store(&state, msg_syn_rst);
                fprintf(stderr, "Remote node rejected connection\n");
                break;
            case msg_pong:
                /* do nothing */
                break;
            case msg_fin_ack:
                atomic_store(&state, msg_fin_ack);
                break;
            case msg_dummy:
                /* do nothing */
                break;
            default:
                fprintf(stderr, "unknown message received\n");
        }
    }
    return NULL;
}

static void *process_client_signal_task(void *arg)
{
    sigset_t sigmask = *(sigset_t *)arg;
    int err;

    if (pthread_sigmask(SIG_BLOCK, &sigmask, NULL) < 0) {
        perror("pthread_sigmask");
        return NULL;
    }

    atomic_store(&signal_initialized, true);
    pthread_mutex_lock(&signal_lock);
    pthread_cond_signal(&signal_cond);
    pthread_mutex_unlock(&signal_lock);
    for (;;) {
        int sig;
        enum nanoping_msg_type s;
        err = sigwait(&sigmask, &sig);
        if (err) {
            errno = err;
            perror("sigwait");
            return NULL;
        }
        switch (sig) {
            case SIGINT:
            case SIGALRM:
                s = atomic_load(&state);
                switch (s) {
                    case msg_none:
                    case msg_syn:
                    case msg_syn_ack:
                    case msg_syn_rst:
                        exit(EXIT_FAILURE);
                    default:
                        atomic_store(&signal_handled, true);
                }
                break;
        }
    }
}

static void *process_txs_task(void *arg)
{
    struct nanoping_instance *ins = (struct nanoping_instance *)arg;

    for (;;)
        nanoping_txs_one(ins);
    return NULL;
}

struct pthread_thread {
    pthread_t thread;
    bool valid;
};

static int close_threads(struct pthread_thread *threads[], size_t n_threads)
{
    int i, err, first_err = 0;

    for (i = 0; i < n_threads; i++) {
        if (!threads[i]->valid)
            continue;

        err = pthread_cancel(threads[i]->thread);
        if (err) {
            errno = err;
            perror("pthread_cancel");
            first_err = first_err ?: -err;
        }

        err = pthread_join(threads[i]->thread, NULL);
        if (err) {
            errno = err;
            perror("pthread_join");
            first_err = first_err ?: -err;
        }

        threads[i]->valid = false;
    }

    return first_err;
}

static int setup_singal_handling(struct pthread_thread *signal_thread)
{
    int err;
    static sigset_t sigmask; // static so that it's still valid for signal thread after this function finishes

    pthread_mutex_init(&signal_lock, NULL);
    pthread_cond_init(&signal_cond, NULL);
    signal_thread->valid = false;

    if (sigemptyset(&sigmask) < 0) {
        perror("sigemptyset");
        return -errno;
    }
    if (sigaddset(&sigmask, SIGINT) < 0) {
        perror("sigaddset(SIGINT)");
        return -errno;
    }
    if (sigaddset(&sigmask, SIGALRM) < 0) {
        perror("sigaddset(SIGALARM)");
        return -errno;
    }
    if (pthread_sigmask(SIG_BLOCK, &sigmask, NULL) < 0) {
        perror("pthread_sigmask");
        return -errno;
    }

    if ((err = pthread_create(&signal_thread->thread, NULL,
                              process_client_signal_task, &sigmask))) {
        errno = err;
        perror("pthread_create(signal_thread)");
        return -err;
    }
    signal_thread->valid = true;

    if (!atomic_load(&signal_initialized)) {
        pthread_mutex_lock(&signal_lock);
        pthread_cond_wait(&signal_cond, &signal_lock);
        pthread_mutex_unlock(&signal_lock);
    }

    return 0;
}

static int setup_txtstamp_thread(struct pthread_thread *txs_thread,
                                 struct nanoping_instance *ins)
{
    int err;

    txs_thread->valid = false;

    if ((err = pthread_create(&txs_thread->thread, NULL, process_txs_task,
                              ins))) {
        errno = err;
        perror("pthread_create(txs_thread)");
        return -err;
    }
    txs_thread->valid = true;

    return 0;
}

static int setup_receive_thread(struct pthread_thread *receive_thread,
                                struct nanoping_instance *ins)
{
    int err;

    receive_thread->valid = false;

    if ((err = pthread_create(&receive_thread->thread, NULL,
                              process_client_receive_task, ins))) {
        errno = err;
        perror("pthread_create(receive_thread)");
        return -err;
    }
    receive_thread->valid = true;

    return 0;
}

static int setup_client_threads(struct nanoping_instance *ins,
                                struct pthread_thread *signal_thread,
                                struct pthread_thread *txs_thread,
                                struct pthread_thread *receive_thread)
{
    struct pthread_thread *threads[] = {signal_thread, txs_thread, receive_thread};
    int err;

    /*
     * setup_signal_handling() needs to run before other threads are created,
     * because it sets the signal mask for the base-thread which other threads
     * will inherit.
     */
    err = setup_singal_handling(signal_thread);
    if (err)
        return err;

    err = setup_txtstamp_thread(txs_thread, ins);
    if (err)
        goto err_threads;

    err = setup_receive_thread(receive_thread, ins);
    if (err)
        goto err_threads;

    return 0;

err_threads:
    close_threads(threads, ARRAY_SIZE(threads));
    return err;
}

static int setup_server_threads(struct nanoping_instance *ins,
                                struct pthread_thread *txs_thread)
{
    return setup_txtstamp_thread(txs_thread, ins);
}

static int client_handshake(const struct sockaddr_in *remaddr,
                            struct nanoping_instance *ins,
                            struct nanoping_client_opts *client_opts)
{
    struct nanoping_send_request send_request = {0};
    bool first_time = true;
    int err;

    memcpy(&send_request.remaddr, remaddr, sizeof(send_request.remaddr));
    send_request.type = msg_syn;
    send_request.seq = 0;

    atomic_store(&state, msg_syn);
    while (atomic_load(&state) == msg_syn) {
        if ((err = nanoping_send_one(ins, &send_request, client_opts,
                                     sizeof(*client_opts)) < 0)) {
            return err;
        }

        if (first_time) {
            first_time = false;
            usleep(50000);
        } else {
            usleep(500000);
        }
    }

    if (atomic_load(&state) != msg_syn_ack)
        return -EBADE;
    atomic_store(&state, client_opts->reverse ? msg_pong : msg_ping);

    return 0;
}

static int client_fin(const struct sockaddr_in *remaddr, uint64_t request_num,
                      struct nanoping_instance *ins)
{
    struct nanoping_send_request send_request;
    bool first_time = true;
    int err;

    memcpy(&send_request.remaddr, remaddr, sizeof(send_request.remaddr));
    send_request.type = msg_fin;
    send_request.seq = request_num;

    atomic_store(&state, msg_fin);
    while (atomic_load(&state) == msg_fin) {
        if ((err = nanoping_send_one(ins, &send_request, NULL, 0)) < 0)
            return err;

        if (first_time) {
            first_time = false;
            usleep(50000);
        } else {
            usleep(500000);
        }
    }

    if (atomic_load(&state) != msg_fin_ack)
        return -EBADE;

    return 0;
}

static int wait_until(uint64_t time_ns, enum timer_type ttype)
{
    uint64_t now = clock_gettime_ns(CLOCK_MONOTONIC);

    if (now > time_ns)
        return 0;

    switch (ttype) {
        case timer_sleep:
            if (usleep((time_ns - now) / NS_PER_US) < 0)
                return -errno;
            break;
        case timer_busy:
            while (clock_gettime_ns(CLOCK_MONOTONIC) < time_ns);
            break;
        default:
            return -EINVAL;
    }

    return 0;
}

static int wait_until_next_interval(uint64_t start_ns, uint64_t interval_ns,
                                    enum timer_type ttype)
{
    uint64_t now, next_interval;

    now = clock_gettime_ns(CLOCK_MONOTONIC);

    if (now < start_ns)
        return -ETIME;

    next_interval = now - ((now - start_ns) % interval_ns);
    next_interval += interval_ns;

    return wait_until(next_interval, ttype);
}

static int client_sendloop(const struct sockaddr_in *remaddr,
                           struct nanoping_instance *ins, int delay_us,
                           enum timer_type ttype, int dummy_pkt,
                           uint64_t *next_seq, ssize_t *sent_pktsize)
{
    struct nanoping_send_request send_request;
    ssize_t siz, pktsize = 0;
    uint64_t start_send, i;
    int res;

    memcpy(&send_request.remaddr, remaddr, sizeof(send_request.remaddr));
    send_request.type = msg_ping;
    start_send = clock_gettime_ns(CLOCK_MONOTONIC);

    for (i = 1; !atomic_load(&signal_handled); i++) {
        send_request.seq = i;
        siz = nanoping_send_one(ins, &send_request, NULL, 0);
        if (siz < 0)
            return siz;

        if (!pktsize)
            pktsize = siz;

        if (dummy_pkt > 0) {
            struct nanoping_send_dummies_request dummies_request;
            memcpy(&dummies_request.remaddr, remaddr,
                   sizeof(dummies_request.remaddr));
            dummies_request.nmsg = dummy_pkt;

            res = nanoping_send_dummies(ins, &dummies_request);
            if (res <= 0)
                return res;
        }

        if (delay_us > 0) {
            res = wait_until_next_interval(start_send, delay_us * NS_PER_US,
                                           ttype);
            if (res < 0 && res != EINTR) {
                fprintf(stderr, "Failed waiting until next interval: %s\n",
                        strerror(-res));
                return res;
            }
        }

    }

    if (next_seq)
        *next_seq = i;
    if (sent_pktsize)
        *sent_pktsize = pktsize;
    return 0;
}

static int run_client_ping_sequence(struct nanoping_instance *ins,
                                    struct sockaddr_in *remaddr,
                                    int delay, enum timer_type ttype,
                                    int dummy_pkt, ssize_t *packet_size)
{
    uint64_t next_seq;
    int res;

    res = client_sendloop(remaddr, ins, delay, ttype, dummy_pkt, &next_seq,
                          packet_size);
    if (res)
        return res;

    res = client_fin(remaddr, next_seq, ins);
    if (res)
        return res;

    return 0;

}

static void prepare_server_reply(struct nanoping_send_request *send_request,
                                 const struct nanoping_receive_result *receive_result,
                                 enum nanoping_msg_type msg_type) {
    send_request->seq = receive_result->seq;
    send_request->remaddr = receive_result->remaddr;
    send_request->type = msg_type;
}

static enum timer_type select_default_timer(int delay_us)
{
    return delay_us <= 1000 ? timer_busy : timer_sleep;
}

static int sanitize_clientopts(struct nanoping_client_opts *opts, bool fix)
{
    int err = 0;

    // If client doesn't attempt to use reverse mode, any values acceptable
    if (!opts->reverse)
        return 0;

    if (opts->count == 0) {
        err = -EINVAL;
        fprintf(stderr, "Warning: Client has not set count (must be non-zero)\n");
        if (fix) {
            fprintf(stderr, "Using default %d s\n", REVERSE_DEFAULT_COUNT);
            opts->count = REVERSE_DEFAULT_COUNT;
        } else {
            return err;
        }
    } else if (opts->count > REVERSE_MAX_COUNT) {
        err = -ERANGE;
        fprintf(stderr, "Warning: Client has set count %u s, must be <= %u s\n",
                opts->count, REVERSE_MAX_COUNT);
        if (fix) {
            fprintf(stderr, "Limiting to %u s\n", REVERSE_MAX_COUNT);
            opts->count = REVERSE_MAX_COUNT;
        } else {
            return err;
        }
    }

    if (opts->delay > REVERSE_MAX_DELAY) {
        err = -ERANGE;
        fprintf(stderr, "Warning: Client has set delay to %u us, must be <= %u us\n",
                opts->delay, REVERSE_MAX_DELAY);
        if (fix) {
            fprintf(stderr, "Limiting to %u us\n", REVERSE_MAX_DELAY);
            opts->delay = REVERSE_MAX_DELAY;
        } else {
            return err;
        }
    }

    if (opts->ttype < 0 || opts->ttype >= timer_invalid) {
        err = -EINVAL;
        fprintf(stderr, "Warning: Client set invalid timer type (%d)\n",
                opts->ttype);
        if (fix) {
            opts->ttype = select_default_timer(opts->delay);
            fprintf(stderr, "Using timer %s (%d) instead\n",
                    timertype_to_str(opts->ttype), opts->ttype);
        } else {
            return err;
        }
    }

    return err;
}

static int server_handshake(struct nanoping_instance *ins,
                            struct sockaddr_in *remaddr,
                            struct nanoping_client_opts *client_opts)
{
    struct nanoping_receive_result receive_result = {0};
    struct nanoping_send_request send_request = {0};
    size_t opt_len = sizeof(*client_opts);
    char buf[INET6_ADDRSTRLEN] = "";
    int res;

    // Wait for SYN
    while (true) {
        memset(&receive_result, 0, sizeof(receive_result));

        nanoping_wait_for_receive(ins);
        res = nanoping_receive_one(ins, &receive_result, client_opts, &opt_len);
        if (res < 0)
            return res;

        if (receive_result.type != msg_syn)
            fprintf(stderr, "Packet of unexpected type (%d) while waiting for SYN, ignoring\n",
                    receive_result.type);
        else if (opt_len < sizeof(*client_opts))
            fprintf(stderr, "Received malformed SYN packet, ignoring\n");
        else
            break;
    }

    // Send SYN-ACK
    prepare_server_reply(&send_request, &receive_result, msg_syn_ack);
    res = nanoping_send_one(ins, &send_request, NULL, 0);
    if (res < 0)
        return res;

    inet_ntop(receive_result.remaddr.sin_family,
              &receive_result.remaddr.sin_addr, buf, sizeof(buf));
    printf("Connected to client %s:%u\n", buf,
           ntohs(receive_result.remaddr.sin_port));

    sanitize_clientopts(client_opts, true);
    printf("Client using settings: count: %u s, delay: %u us, timer: %s (%d), reverse: %s\n",
           client_opts->count, client_opts->delay,
           timertype_to_str(client_opts->ttype), client_opts->ttype,
           client_opts->reverse ? "true" : "false");

    atomic_store(&state, client_opts->reverse ? msg_ping : msg_pong);
    *remaddr = receive_result.remaddr;
    return 0;
}

static int server_handle_foreign_packet(struct nanoping_instance *ins,
                                        const struct nanoping_receive_result *receive_result)
{
    struct nanoping_send_request send_request = {0};
    char ip_str[INET6_ADDRSTRLEN] = "";
    char *type_str;
    int res;

    if (receive_result->type == msg_syn) {
        prepare_server_reply(&send_request, receive_result, msg_syn_rst);
        res = nanoping_send_one(ins, &send_request, NULL, 0);
        if (res < 0)
            return res;

        type_str = "new connection request";
    } else {
        // Ignore non-SYN packets from non-connected clients
        type_str = "packet";
    }

    inet_ntop(receive_result->remaddr.sin_family,
              &receive_result->remaddr.sin_addr, ip_str, sizeof(ip_str));
    fprintf(stderr, "Connection already established, drop %s from %s:%u.\n",
            type_str, ip_str, ntohs(receive_result->remaddr.sin_port));

    return 0;
}

static int server_handle_ping(struct nanoping_instance *ins,
                              const struct nanoping_receive_result *receive_result,
                              int dummy_pkt, ssize_t *pktsize)
{
    struct nanoping_send_dummies_request dummies_request;
    struct nanoping_send_request send_request = {0};
    int res;

    switch (receive_result->type) {
        case msg_syn:
            // SYN-ACK to client must have been lost, resend
            prepare_server_reply(&send_request, receive_result, msg_syn_ack);
            res = nanoping_send_one(ins, &send_request, NULL, 0);
            if (res < 0)
                return res;
            break;
        case msg_fin:
            prepare_server_reply(&send_request, receive_result, msg_fin_ack);
            res = nanoping_send_one(ins, &send_request, NULL, 0);
            if (res < 0)
                return res;

            atomic_store(&state, msg_none);
            printf("Client disconnected.\n");
            break;
        case msg_ping:
            if (atomic_load(&state) != msg_pong) {
                fprintf(stderr, "received message (msg_ping) is inconsistent with current state, ignoring\n");
                break;
            }
            prepare_server_reply(&send_request, receive_result, msg_pong);
            res = nanoping_send_one(ins, &send_request, NULL, 0);
            if (res < 0)
                return res;
            if (pktsize)
                *pktsize = res;

            if (dummy_pkt) {
                dummies_request.remaddr = receive_result->remaddr;
                dummies_request.nmsg = dummy_pkt;

                res = nanoping_send_dummies(ins, &dummies_request);
                if (res <= 0)
                    return res;
            }
            break;
        case msg_dummy:
            /* do nothing */
            break;
        default:
            fprintf(stderr, "Illegal packet received, ignoring.\n");
    }

    return receive_result->type;
}

static int server_echoloop(struct nanoping_instance *ins,
                           const struct sockaddr_in *remaddr, int dummy_pkt,
                           ssize_t *sent_pktsize)
{
    struct nanoping_receive_result receive_result;
    ssize_t pktsize = 0;
    int res;

    while (atomic_load(&state) == msg_pong) {
        memset(&receive_result, 0, sizeof(receive_result));

        res = nanoping_receive_one(ins, &receive_result, NULL, NULL);
        if (res < 0)
            return res;

        if (memcmp(&receive_result.remaddr, remaddr, sizeof(*remaddr)) == 0)
            res = server_handle_ping(ins, &receive_result, dummy_pkt,
                                 pktsize == 0 ? &pktsize : NULL);
        else
            res = server_handle_foreign_packet(ins, &receive_result);
        if (res < 0)
            return res;
    }

    if (sent_pktsize)
        *sent_pktsize = pktsize;

    return 0;
}

static int run_client(struct nanoping_instance *ins, char *host, char *port,
                      int dummy_pkt, struct nanoping_client_opts *client_opts)
{
    struct addrinfo *reminfo;
    struct pthread_thread signal_thread = {0};
    struct pthread_thread txs_thread = {0};
    struct pthread_thread receive_thread = {0};
    struct pthread_thread *threads[] = {&signal_thread, &txs_thread, &receive_thread};
    struct timespec started, finished, duration;
    ssize_t pktsize = 0;
    int err;

    err = getaddrinfo(host, port, NULL, &reminfo);
    if (err) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
        return EXIT_FAILURE;
    }

    err = setup_client_threads(ins, &signal_thread, &txs_thread, &receive_thread);
    if (err) {
        fprintf(stderr, "Failed setting up client threads: %s\n", strerror(-err));
        return EXIT_FAILURE;
    }

    if (client_opts->count > 0) {
        alarm(client_opts->count);
    }

    printf("nanoping %s:%s...\n", host, port);

    err = client_handshake((struct sockaddr_in *)reminfo->ai_addr, ins,
                           client_opts);
    if (err)
        return EXIT_FAILURE;

    if (clock_gettime(CLOCK_MONOTONIC, &started)) {
        perror("clock_gettime(started)");
        return EXIT_FAILURE;
    }

    if (client_opts->reverse) {
        err = close_threads(threads, ARRAY_SIZE(threads));
        err = err ?: setup_server_threads(ins, &txs_thread);
        err = err ?: server_echoloop(ins, (struct sockaddr_in *)reminfo->ai_addr,
                                     dummy_pkt, &pktsize);
    } else {
        err = run_client_ping_sequence(ins, (struct sockaddr_in *)reminfo->ai_addr,
                                       client_opts->delay, client_opts->ttype,
                                       dummy_pkt, &pktsize);
    }
    if (err) {
        fprintf(stderr, "Failed %s pings: %s\n",
                client_opts->reverse ? "echoing" : "sending", strerror(-err));
        return EXIT_FAILURE;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &finished)) {
        perror("clock_gettime");
        return EXIT_FAILURE;
    }
    timevalsub(&finished, &started, &duration);
    dump_statistics(ins, &duration, true, pktsize);

    close_threads(threads, ARRAY_SIZE(threads));

    return EXIT_SUCCESS;
}

static int run_server(struct nanoping_instance *ins, char *port, int dummy_pkt)
{
    struct pthread_thread signal_thread = {0};
    struct pthread_thread txs_thread = {0};
    struct pthread_thread receive_thread = {0};
    struct pthread_thread *threads[] = {&signal_thread, &txs_thread, &receive_thread};
    struct timespec started, finished, duration;
    struct nanoping_client_opts client_opts;
    struct sockaddr_in remaddr;
    ssize_t pktsize = 0;
    int err;

    err = setup_server_threads(ins, &txs_thread);
    if (err) {
        fprintf(stderr, "Failed setting up server threads: %s\n", strerror(-err));
        return EXIT_FAILURE;
    }

    printf("nanoping server started on port %s.\n", port);

    err = server_handshake(ins, &remaddr, &client_opts);
    if (err)
        return EXIT_FAILURE;

    if (clock_gettime(CLOCK_MONOTONIC, &started)) {
        perror("clock_gettime");
        return EXIT_FAILURE;
    }

    if (client_opts.reverse) {
        err = close_threads(threads, ARRAY_SIZE(threads));
        err = err ?: setup_client_threads(ins, &signal_thread, &txs_thread,
                                          &receive_thread);
        sleep(1); // Give client some time to setup
        alarm(client_opts.count);
        err = err ?: run_client_ping_sequence(ins, &remaddr, client_opts.delay,
                                              client_opts.ttype, dummy_pkt,
                                              &pktsize);
    } else {
        err = server_echoloop(ins, &remaddr, dummy_pkt, &pktsize);
    }
    if (err) {
        fprintf(stderr, "Failed %s pings: %s\n",
                client_opts.reverse ? "sending" : "echoing", strerror(-err));
        return EXIT_FAILURE;
    }

    // Test finished - shutdown
    if (clock_gettime(CLOCK_MONOTONIC, &finished)) {
        perror("clock_gettime");
        return EXIT_FAILURE;
    }
    timevalsub(&finished, &started, &duration);
    dump_statistics(ins, &duration, true, pktsize);

    close_threads(threads, ARRAY_SIZE(threads));

    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    struct nanoping_instance *ins = NULL;
    struct nanoping_client_opts client_opts = {
        .count = 0, .delay = 100, .ttype = timer_invalid, .reverse = false,
    };
    enum nanoping_mode mode = mode_none;
    char *interface = NULL;
    char *host = NULL;
    char *port = "10666";
    char *log = NULL;
    bool emulation = false;
    int timeout = 5000000;
    int busy_poll = 0;
    int dummy_pkt = 0;
    int pad_bytes = 0;
    int c, res, nargc = argc;

    if (argc >= 2) {
        if (!strcmp(argv[1], "--server")) {
            mode = mode_server;
            nargc--;
        } else if (!strcmp(argv[1], "--client")) {
            mode = mode_client;
            nargc -= 2;
        } else {
            usage();
            return EXIT_FAILURE;
        }
    } else {
	usage();
	return EXIT_FAILURE;
    }
    while ((c = getopt_long(nargc, argv + 1, "i:n:d:p:l:et:B:b:T:Rh", longopts, NULL)) != -1) {
        switch (c) {
            case 'i':
                interface = optarg;
                break;
            case 'n':
                client_opts.count = atoi(optarg);
                break;
            case 'd':
                client_opts.delay = atoi(optarg);
                break;
            case 'p':
                port = optarg;
                break;
            case 'l':
                log = optarg;
                break;
            case 'e':
                emulation = true;
                break;
            case 't':
                timeout = atoi(optarg);
                break;
            case 'B':
                pad_bytes = atoi(optarg);
                break;
            case 'b':
                busy_poll = atoi(optarg);
                break;
            case 'x':
                dummy_pkt = atoi(optarg);
                break;
            case 'T':
                client_opts.ttype = str_to_timertype(optarg);
                if (client_opts.ttype == timer_invalid) {
                    fprintf(stderr, "Unrecognized timer type %s. Available options are:\n",
                            optarg);
                    for (enum timer_type ttype = 0; ttype < timer_n_types; ttype++)
                        fprintf(stderr, "%s\n", timertype_to_str(ttype));
                    return EXIT_FAILURE;
                }
                break;
            case 'R':
                client_opts.reverse = true;
                break;
            case 'h':
            default:
                usage();
                return EXIT_FAILURE;
        }
    }

    if (pad_bytes < 0 || pad_bytes > MAX_PAD_BYTES) {
        fprintf(stderr, "pad-bytes must be in range [%d, %d]\n",
                0, MAX_PAD_BYTES);
        return EXIT_FAILURE;
    }

    // If timer-type not set, automatically set it based on configured delay
    if (client_opts.ttype == timer_invalid)
        client_opts.ttype = select_default_timer(client_opts.delay);

    // Prevent client from starting with invalid client-options
    res = sanitize_clientopts(&client_opts, false);
    if (res) {
        fprintf(stderr, "Invalid client option: %s\n", strerror(-res));
        return EXIT_FAILURE;
    }

    if (mode == mode_client) {
        host = argv[argc - 1];
        if (!interface || !host) {
            usage();
            return EXIT_FAILURE;
        }
    } else if (mode == mode_server) {
        if (!interface) {
            usage();
            return EXIT_FAILURE;
        }
     }

    if ((ins = nanoping_init(interface, port, mode == mode_server ? true: false,
                             emulation, timeout, pad_bytes, busy_poll, log)) == NULL) {
        return EXIT_FAILURE;
    }

    if (mode == mode_client) {
        res = run_client(ins, host, port, dummy_pkt, &client_opts);
    } else {
        res = run_server(ins, port, dummy_pkt);
    }
    nanoping_finish(ins);
    return res;
}
