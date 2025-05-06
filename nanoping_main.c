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
#define CLIENT_MAX_COUNT 3600 // 1h
#define CLIENT_MAX_DELAY 100000000 // 100sec

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

enum test_direction {
    test_forward = 0,
    test_reverse,
    test_duplex,
    test_n_directions,
};

struct probe_entry {
    uint32_t delay;
    uint16_t pad_bytes;
};

struct probe_schedule {
    struct probe_entry *entries;
    size_t len;
};

struct nanoping_client_opts {
    uint32_t count;
    uint32_t delay;
    uint16_t ping_pad;
    uint16_t pong_pad;
    uint32_t pong_every;
    enum timer_type ttype;
    enum test_direction direction;
};

static struct option longopts[] = {
    {"interface",      required_argument, NULL, 'i'},
    {"count",          required_argument, NULL, 'n'},
    {"delay",          required_argument, NULL, 'd'},
    {"port",           required_argument, NULL, 'p'},
    {"log",            required_argument, NULL, 'l'},
    {"emulation",      no_argument,       NULL, 'e'},
    {"timeout",        required_argument, NULL, 't'},
    {"busypoll",       required_argument, NULL, 'b'},
    {"dummy-pkt",      required_argument, NULL, 'x'},
    {"ping-size",      required_argument, NULL, 's'},
    {"pong-size",      required_argument, NULL, 'o'},
    {"timer",          required_argument, NULL, 'T'},
    {"reverse",        no_argument,       NULL, 'R'},
    {"duplex",         no_argument,       NULL, 'D'},
    {"probe-schedule", required_argument, NULL, 'S'},
    {"pong-every",     required_argument, NULL, 'y'},
    {"help",           no_argument,       NULL, 'h'},
    {0,                0,                 0,     0 }
};

static atomic_bool signal_initialized = ATOMIC_VAR_INIT(false);
static atomic_bool signal_handled = ATOMIC_VAR_INIT(false);
static _Atomic enum nanoping_msg_type state = ATOMIC_VAR_INIT(msg_none);
static pthread_mutex_t signal_lock;
static pthread_mutex_t ping_wait_lock;
static pthread_cond_t signal_cond;
static pthread_cond_t ping_wait_cond;
static void usage(void)
{
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "  client: nanoping --client --interface [nic] --count [sec] --delay [usec] --port [port] --log [logfile] --emulation --timeout [usec] --busypoll [usec] --dummy-pkt [cnt] --timer [timer-type] --ping-size [bytes] --pong-size [bytes] --probe-schedule [csv] --pong-every [n] --reverse/--duplex [host]\n");
    fprintf(stderr, "  server: nanoping --server --interface [nic] --port [port] --log [logfile] --emulation --timeout [usec] --busypoll [usec] --dummy-pkt [cnt]\n");
}

inline static double percent_ulong(unsigned long v1, unsigned long v2)
{
    return (((double)(v1 - v2)) * 100) / v1;
}

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

static const char *testdirection_to_str(enum test_direction direction)
{
    switch (direction) {
        case test_forward:
            return "forward";
        case test_reverse:
            return "reverse";
        case test_duplex:
            return "duplex";
        default:
            return "invalid";
    }
}

static void dump_statistics(struct nanoping_instance *ins,
                            struct timespec *duration, size_t size,
                            bool calc_pktloss)
{
    assert(ins);
    assert(duration);

    double duration_s = duration->tv_sec + (double)duration->tv_nsec / NS_PER_S;

    printf("--- nanoping statistics ---\n");
    printf("packet size %zd bytes, time %ld.%09ld s\n",
           size + TOT_NETHDR_SIZE, duration->tv_sec, duration->tv_nsec);
    printf("%lu packets transmitted (%.1f pps), %lu received (%.1f pps)",
           ins->pkt_transmitted, ins->pkt_transmitted / duration_s,
           ins->pkt_received, ins->pkt_received / duration_s);
    if (calc_pktloss)
	    printf(", %f%% packet loss\n",
            percent_ulong(ins->pkt_transmitted, ins->pkt_received));
    else
        printf("\n");
    printf("%lu TX stamp collected (%.2f%%), %lu RX stamp collected (%.2f%%)\n",
           ins->txs_collected,
           (double)ins->txs_collected / ins->pkt_transmitted * 100,
           ins->rxs_collected,
           (double)ins->rxs_collected / ins->pkt_received * 100);
}

static bool valid_packetsize(int pkt_size, bool verbose)
{
    if (pkt_size < TOT_APPNETHDR_SIZE ||
        pkt_size > TOT_APPNETHDR_SIZE + MAX_PAD_BYTES) {
        if (verbose)
            fprintf(stderr, "packet size %d invalid, must be in range [%lu, %lu]\n",
                    pkt_size, TOT_APPNETHDR_SIZE,
                    TOT_APPNETHDR_SIZE + MAX_PAD_BYTES);
        return false;
    }

    return true;
}

static int parse_bounded_integer(long long *val, const char *str,
                                 long long val_min, long long val_max)
{
    char *end;

    errno = 0;
    *val = strtoll(str, &end, 10);
    if (errno)
        return -errno;

    if (end == str) // Didn't parse anything
        return -EINVAL;

    if (*val < val_min || *val > val_max)
        return -ERANGE;

    return 0;
}

static int count_char_in_str(const char *str, char c, size_t n)
{
    int i, count = 0;

    for (i = 0; i < n; i++) {
        if (str[i] == '\0')
            break;

        if (str[i] == c)
            count++;
    }

    return count;
}

static int count_file_lines(const char *path)
{
    char buf[4096];
    int lines = 0;
    FILE *stream;
    size_t len;


    stream = fopen(path, "r");
    if (!stream)
        return -errno;

    while ((len = fread(buf, 1, sizeof(buf), stream)) > 0)
        lines += count_char_in_str(buf, '\n', len);

    fclose(stream);
    return lines;
}

static int parse_probesched_csv_line(char *line, struct probe_entry *entry)
{
    char *word;
    long long val;
    int n, err;

    n = count_char_in_str(line, ',', strlen(line));
    if (n != 1) {
        fprintf(stderr, "Error: Found %d CSV fields, expected 2\n", n + 1);
        return -EINVAL;
    }

    word = strtok(line, ",");
    if (!word) {
        fprintf(stderr, "Error: No delay field\n");
        return -EINVAL;
    }

    err = parse_bounded_integer(&val, word, 0, (1ULL << 32) - 1);
    if (err) {
        fprintf(stderr, "Error: %s not a valid delay value\n",
                word);
        return err;
    }
    entry->delay = val;

    word = strtok(NULL, ",");
    if (!word) {
        fprintf(stderr, "Error: No packetsize field\n");
        return -EINVAL;
    }

    err = parse_bounded_integer(&val, word, 0, (1ULL << 16));
    if (err) {
        fprintf(stderr, "Error: %s not a valid packetsize value\n", word);
        return err;
    }
    if (!valid_packetsize(val, true))
        return -ERANGE;
    entry->pad_bytes = val - TOT_APPNETHDR_SIZE;

    return 0;
}

static int parse_probesched_csv(const char *path,
                                struct probe_schedule *schedule)
{
    struct probe_entry *data;
    int err, lines, line = 0;
    FILE *stream = NULL;
    char buf[128];
    size_t strl;

    lines = count_file_lines(path);
    if (lines < 0)
        return lines;

    data = calloc(lines, sizeof(*data));
    if (!data)
        return -ENOMEM;

    stream = fopen(path, "r");
    if (!stream)
        return -errno;

    while (fgets(buf, sizeof(buf), stream)) {
        strl = strlen(buf);
        if (strl == 0 || (strl == 1 && buf[0] == '\n'))
            // Empty line - skip
            continue;

        if (strl == sizeof(buf) - 1 && buf[sizeof(buf) - 1] != '\n') {
            fprintf(stderr, "Error: %s line %d is longer than %lu characters\n",
                    path, line, sizeof(buf) - 1);
            err = -E2BIG;
            goto err;
        }

        err = parse_probesched_csv_line(buf, data + line);
        if (err) {
            fprintf(stderr, "Error: Unable to parse %s line %d: %s\n", path,
                    line, strerror(-err));
            goto err;
        }

        line++;
    }

    fclose(stream);
    schedule->entries = data;
    schedule->len = line;
    return 0;

err:
    if (stream)
        fclose(stream);
    free(data);
    return err;
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
            case msg_ping:
                if (atomic_load(&state) == msg_ping_wait)
                    pthread_mutex_lock(&ping_wait_lock);
                    atomic_store(&state, msg_ping);
                    pthread_cond_signal(&ping_wait_cond);
                    pthread_mutex_unlock(&ping_wait_lock);
                break;
            case msg_pong:
            case msg_dummy:
                /* do nothing */
                break;
            case msg_fin_ack:
                atomic_store(&state, msg_fin_ack);
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
    atomic_store(&state, client_opts->direction == test_reverse ? msg_pong :
                                                                  msg_ping);

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
                           struct nanoping_instance *ins,
                           struct nanoping_client_opts *opts,
                           struct probe_schedule *schedule, int dummy_pkt,
                           uint64_t *next_seq, ssize_t *sent_pktsize,
                           struct timespec *start, struct timespec *end)
{
    struct nanoping_send_request send_request;
    uint64_t start_send, start_round, delay, i;
    ssize_t siz, pkt_pad, pktsize = 0;
    int res;

    memcpy(&send_request.remaddr, remaddr, sizeof(send_request.remaddr));
    send_request.type = msg_ping;
    start_send = clock_gettime_ns(CLOCK_MONOTONIC);

    for (i = 1; !atomic_load(&signal_handled); i++) {
        if (schedule) {
            if (i > schedule->len)
                break;

            pkt_pad = schedule->entries[i - 1].pad_bytes;
            delay = schedule->entries[i - 1].delay;
            start_round = clock_gettime_ns(CLOCK_MONOTONIC);
        } else {
            pkt_pad = opts->ping_pad;
            delay = opts->delay;
        }

        send_request.seq = i;
        siz = nanoping_send_one(ins, &send_request, NULL, pkt_pad);
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

        if (delay > 0) {
            if (schedule)
                // For non-fixed delays, the interval algorithm does not work
                res = wait_until(start_round + delay * NS_PER_US, opts->ttype);
            else
                res = wait_until_next_interval(start_send, delay * NS_PER_US,
                                           opts->ttype);
            if (res < 0 && res != EINTR) {
                fprintf(stderr, "Failed waiting for next ping: %s\n",
                        strerror(-res));
                return res;
            }
        }

    }

    if (end)
        clock_gettime(CLOCK_MONOTONIC, end);
    if (start) {
        start->tv_sec = start_send / NS_PER_S;
        start->tv_nsec = start_send % NS_PER_S;
    }
    if (next_seq)
        *next_seq = i;
    if (sent_pktsize)
        *sent_pktsize = pktsize;
    return 0;
}

static int run_client_ping_sequence(struct nanoping_instance *ins,
                                    struct sockaddr_in *remaddr,
                                    struct nanoping_client_opts *opts,
                                    struct probe_schedule *schedule,
                                    int dummy_pkt, ssize_t *packet_size,
                                    struct timespec *start,
                                    struct timespec *end)
{
    uint64_t next_seq;
    int res;

    res = client_sendloop(remaddr, ins, opts, schedule, dummy_pkt, &next_seq,
                          packet_size, start, end);
    if (res)
        return res;

    if (opts->direction == test_duplex) {
        // No handshake for duplex mode
        sleep(1);
    } else {
        res = client_fin(remaddr, next_seq, ins);
        if (res)
            return res;
    }

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

static int64_t sanitize_clientopt(const char *name, int64_t val,
                                  int64_t min_val, int64_t max_val, int *err,
                                  bool fix, const int64_t *fixval,
                                  const char *fixval_name)
{
    int64_t replacement;

    *err = 0;

    if (val < min_val || val > max_val) {
        *err = -ERANGE;
        fprintf(stderr, "Warning: Client has set %s %ld, must be in [%ld, %ld]\n",
                name, val, min_val, max_val);

        if (fix) {
            replacement = fixval ? *fixval : val < min_val ? min_val : max_val;

            if (fixval && fixval_name)
                fprintf(stderr, "Using %s %s (%ld) instead\n", name,
                        fixval_name, replacement);
            else
                fprintf(stderr, "Using %s %ld instead\n", name, replacement);

            return replacement;
        }
    }

    return val;
}

static int sanitize_clientopts(struct nanoping_client_opts *opts,
                               bool has_schedule, bool fix)
{
    bool checkcount, checkdelay, checkpingpad, checkpongpad, checkttype;
    bool checkpongevery;
    int64_t replacement;
    int err = 0;

    replacement = test_forward;
    opts->direction = sanitize_clientopt("test direction", opts->direction, 0,
                                         test_n_directions - 1, &err, fix,
                                         &replacement,
                                         testdirection_to_str(replacement));
    if (!fix && err)
        return err;

    // Determine which fields need to be checked
    checkcount = (opts->direction != test_forward) && !has_schedule;
    checkdelay = (opts->direction != test_forward) && !has_schedule;
    checkpingpad = (opts->direction != test_forward) && !has_schedule;
    checkpongpad = opts->direction == test_forward;
    checkpongevery = opts->direction == test_forward;
    checkttype = opts->direction != test_forward;

    if (checkcount) {
        opts->count = sanitize_clientopt("count", opts->count, 1,
                                         CLIENT_MAX_COUNT, &err, fix, NULL, NULL);
        if (!fix && err)
            return err;
    }

    if (checkdelay) {
        opts->delay = sanitize_clientopt("delay", opts->delay, 0,
                                         CLIENT_MAX_DELAY, &err, fix, NULL, NULL);
        if (!fix && err)
            return err;
    }

    if (checkpingpad) {
        opts->ping_pad = sanitize_clientopt("ping padding", opts->ping_pad, 0,
                                            MAX_PAD_BYTES, &err, fix, NULL, NULL);
        if (!fix && err)
            return err;
    }

    if (checkpongpad) {
        opts->pong_pad = sanitize_clientopt("pong padding", opts->pong_pad, 0,
                                            MAX_PAD_BYTES, &err, fix, NULL, NULL);
        if (!fix && err)
            return err;

    }

    if (checkpongevery) {
        opts->pong_every = sanitize_clientopt("pong every", opts->pong_every, 0,
                                              1000000, &err, fix, NULL, NULL);
        if (!fix && err)
            return err;
    }

    if (checkttype) {
        replacement = select_default_timer(opts->delay);
        opts->ttype = sanitize_clientopt("timer type", opts->ttype, 0,
                                         timer_n_types - 1, &err, fix, &replacement,
                                         timertype_to_str(replacement));
        if (!fix && err)
            return err;
    }

    return 0;
}

static void server_log_client_connection(const struct sockaddr_in *remaddr)
{
    char buf[INET6_ADDRSTRLEN] = "";

    inet_ntop(remaddr->sin_family, &remaddr->sin_addr, buf, sizeof(buf));
    printf("Connected to client %s:%u\n", buf, ntohs(remaddr->sin_port));
}

static void server_log_clientopts(const struct nanoping_client_opts *client_opts)
{
    printf("Client using settings: count: %u s, delay: %u us, ping-padding %u bytes, pong-padding %u bytes, pong-every: %u, timer: %s (%d), direction: %s (%d)\n",
           client_opts->count, client_opts->delay, client_opts->ping_pad,
           client_opts->pong_pad, client_opts->pong_every,
           timertype_to_str(client_opts->ttype), client_opts->ttype,
           testdirection_to_str(client_opts->direction),
           client_opts->direction);
}

static int server_handshake(struct nanoping_instance *ins,
                            struct sockaddr_in *remaddr,
                            struct nanoping_client_opts *client_opts,
                            bool has_schedule)
{
    struct nanoping_receive_result receive_result = {0};
    struct nanoping_send_request send_request = {0};
    size_t opt_len = sizeof(*client_opts);
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

    server_log_client_connection(&receive_result.remaddr);

    sanitize_clientopts(client_opts, has_schedule, true);
    server_log_clientopts(client_opts);

    if (client_opts->direction == test_duplex)
        ins->log_pktdir = true;
    atomic_store(&state, client_opts->direction == test_duplex ? msg_ping_wait :
                         client_opts->direction == test_reverse ? msg_ping :
                                                                  msg_pong);
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
                              const struct nanoping_client_opts *client_opts,
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
            if (client_opts->pong_every == 0 ||
                receive_result->seq % client_opts->pong_every != 0)
                break;

            prepare_server_reply(&send_request, receive_result, msg_pong);
            res = nanoping_send_one(ins, &send_request, NULL, client_opts->pong_pad);
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
                           const struct sockaddr_in *remaddr,
                           const struct nanoping_client_opts *client_opts,
                           int dummy_pkt, ssize_t *sent_pktsize,
                           struct timespec *start, struct timespec *end)
{
    struct nanoping_receive_result receive_result;
    uint64_t first_rcv = 0;
    ssize_t pktsize = 0;
    int res;

    while (atomic_load(&state) == msg_pong) {
        memset(&receive_result, 0, sizeof(receive_result));

        res = nanoping_receive_one(ins, &receive_result, NULL, NULL);
        if (res < 0)
            return res;

        if (memcmp(&receive_result.remaddr, remaddr, sizeof(*remaddr)) == 0) {
            res = server_handle_ping(ins, &receive_result, client_opts, dummy_pkt,
                                     pktsize == 0 ? &pktsize : NULL);

            if (first_rcv == 0 && receive_result.type == msg_ping)
                first_rcv = clock_gettime_ns(CLOCK_MONOTONIC);
        } else {
            res = server_handle_foreign_packet(ins, &receive_result);
        }
        if (res < 0)
            return res;
    }

    if (end)
        clock_gettime(CLOCK_MONOTONIC, end);
    if (start) {
        start->tv_sec = first_rcv / NS_PER_S;
        start->tv_nsec = first_rcv % NS_PER_S;
    }
    if (sent_pktsize)
        *sent_pktsize = pktsize;
    return 0;
}

static void wait_for_ping(enum timer_type ttype)
{
    if (ttype == timer_busy) {
        while (atomic_load(&state) == msg_ping_wait);
    } else {
        pthread_mutex_lock(&ping_wait_lock);
        while (atomic_load(&state) == msg_ping_wait)
            pthread_cond_wait(&ping_wait_cond, &ping_wait_lock);
        pthread_mutex_unlock(&ping_wait_lock);
    }
}

static int run_client(struct nanoping_instance *ins, char *host, char *port,
                      int dummy_pkt, struct nanoping_client_opts *client_opts,
                      struct probe_schedule *schedule)
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

    printf("nanoping %s:%s...\n", host, port);

    err = client_handshake((struct sockaddr_in *)reminfo->ai_addr, ins,
                           client_opts);
    if (err)
        return EXIT_FAILURE;

    if (client_opts->direction == test_reverse) {
        err = close_threads(threads, ARRAY_SIZE(threads));
        err = err ?: setup_server_threads(ins, &txs_thread);
        err = err ?: server_echoloop(ins, (struct sockaddr_in *)reminfo->ai_addr,
                                     client_opts, dummy_pkt, &pktsize,
                                     &started, &finished);
    } else {
        // In duplex mode - give server some time to transition to client mode
        if (client_opts->direction == test_duplex)
            sleep(1);

        if (!schedule && client_opts->count > 0)
            alarm(client_opts->count);

        err = run_client_ping_sequence(ins, (struct sockaddr_in *)reminfo->ai_addr,
                                       client_opts, schedule, dummy_pkt, &pktsize,
                                       &started, &finished);
    }
    if (err) {
        fprintf(stderr, "Failed %s pings: %s\n",
                client_opts->direction == test_reverse ? "echoing" : "sending",
                strerror(-err));
        return EXIT_FAILURE;
    }

    timevalsub(&finished, &started, &duration);
    dump_statistics(ins, &duration, pktsize,
                    client_opts->direction == test_forward);

    close_threads(threads, ARRAY_SIZE(threads));

    return EXIT_SUCCESS;
}

static int run_server(struct nanoping_instance *ins, char *port, int dummy_pkt,
                      struct probe_schedule *schedule)
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

    pthread_cond_init(&ping_wait_cond, NULL);
    pthread_mutex_init(&ping_wait_lock, NULL);

    err = setup_server_threads(ins, &txs_thread);
    if (err) {
        fprintf(stderr, "Failed setting up server threads: %s\n", strerror(-err));
        return EXIT_FAILURE;
    }

    printf("nanoping server started on port %s.\n", port);

    err = server_handshake(ins, &remaddr, &client_opts, schedule != NULL);
    if (err)
        return EXIT_FAILURE;

    if (client_opts.direction == test_forward) {
        err = server_echoloop(ins, &remaddr, &client_opts, dummy_pkt,
                              &pktsize, &started, &finished);
    } else {
        // reverse or duplex - switch to client-mode
        err = close_threads(threads, ARRAY_SIZE(threads));
        err = err ?: setup_client_threads(ins, &signal_thread, &txs_thread,
                                          &receive_thread);

        if (client_opts.direction == test_duplex)
            // In duplex mode, wait for first ping from client
            wait_for_ping(client_opts.ttype);
        else
            // In reverse mode, give client some time to transiton to server mode
            sleep(1);

        if (!schedule)
            alarm(client_opts.count);
        err = err ?: run_client_ping_sequence(ins, &remaddr, &client_opts,
                                              schedule, dummy_pkt, &pktsize,
                                              &started, &finished);
    }
    if (err) {
        fprintf(stderr, "Failed %s pings: %s\n",
                client_opts.direction == test_forward ? "echoing" : "sending",
                strerror(-err));
        return EXIT_FAILURE;
    }

    // Test finished - shutdown
    timevalsub(&finished, &started, &duration);
    dump_statistics(ins, &duration, pktsize,
                    client_opts.direction == test_reverse);

    close_threads(threads, ARRAY_SIZE(threads));

    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    struct nanoping_instance *ins = NULL;
    struct nanoping_client_opts client_opts = {
        .count = 0,
        .delay = 100,
        .ping_pad = 0,
        .pong_pad = 0,
        .pong_every = 1,
        .ttype = timer_invalid,
        .direction = test_forward,
    };
    struct probe_schedule schedule = { 0 };
    enum nanoping_mode mode = mode_none;
    char *interface = NULL;
    char *host = NULL;
    char *port = "10666";
    char *log = NULL;
    bool emulation = false;
    int timeout = 5000000;
    int busy_poll = 0;
    int dummy_pkt = 0;
    int c, res, nargc = argc;
    bool reverse = false, duplex = false;

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
    while ((c = getopt_long(nargc, argv + 1, "i:n:d:p:l:et:s:o:b:T:RDS:y:h", longopts, NULL)) != -1) {
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
            case 's':
                res = atoi(optarg);
                if (valid_packetsize(res, true))
                    client_opts.ping_pad = res - TOT_APPNETHDR_SIZE;
                else
                    return EXIT_FAILURE;
                break;
            case 'o':
                res = atoi(optarg);
                if (valid_packetsize(res, true))
                    client_opts.pong_pad = res - TOT_APPNETHDR_SIZE;
                else
                    return EXIT_FAILURE;
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
                // Don't set client_opts.direction directly to detect if both reverse and duplex are set
                reverse = true;
                break;
            case 'D':
                duplex = true;
                break;
            case 'S':
                if (parse_probesched_csv(optarg, &schedule) != 0 ||
                    schedule.len == 0) {
                    fprintf(stderr, "Failed parsing CSV %s\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 'y':
                client_opts.pong_every = atoi(optarg);
                break;
            case 'h':
            default:
                usage();
                return EXIT_FAILURE;
        }
    }

    // If timer-type not set, automatically set it based on configured delay
    if (client_opts.ttype == timer_invalid)
        client_opts.ttype = select_default_timer(client_opts.delay);

    if (reverse && duplex) {
        fprintf(stderr, "Reverse and duplex mode are mutally exclusive\n");
        return EXIT_FAILURE;
    } else if (reverse) {
        client_opts.direction = test_reverse;
    } else if (duplex)
        client_opts.direction = test_duplex;
    else {
        client_opts.direction = test_forward;
    }

    // Prevent client from starting with invalid client-options
    res = sanitize_clientopts(&client_opts, false, false);
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
                             emulation, timeout, busy_poll, log,
                             client_opts.direction == test_duplex)) == NULL) {
        return EXIT_FAILURE;
    }

    if (mode == mode_client) {
        res = run_client(ins, host, port, dummy_pkt, &client_opts,
                         schedule.len > 0 ? &schedule : NULL);
    } else {
        res = run_server(ins, port, dummy_pkt,
                         schedule.len > 0 ? &schedule : NULL);
    }
    nanoping_finish(ins);
    return res;
}
