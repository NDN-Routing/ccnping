/*
 * ccnping sends ping Interests towards a name prefix to test connectivity.
 * Copyright (C) 2011 University of Arizona
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Author: Cheng Yi <yic@email.arizona.edu>
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <sys/time.h>
#include <signal.h>
#include <limits.h>
#include <math.h>
#include <ccn/ccn.h>
#include <ccn/uri.h>
#include <ccn/schedule.h>
#include <ccn/hashtb.h>

#define PING_COMPONENT "ping"
#define PING_MIN_INTERVAL 0.1

struct ccn_ping_client {
    char *original_prefix;              //name prefix given by command line
    struct ccn_charbuf *prefix;         //name prefix to ping
    double interval;                    //interval between pings in seconds
    int sent;                           //number of interest sent
    int received;                       //number of content or timeout received
    int total;                          //total number of pings to send
    long int number;                    //the number used in ping Interest name, number < 0 means random
    struct ccn *h;
    struct ccn_schedule *sched;
    struct ccn_scheduled_event *event;
    struct ccn_closure *closure;
    struct hashtb *ccn_ping_table;
};

struct ccn_ping_entry {
    long int number;
    struct timeval send_time;
};

struct ccn_ping_statistics {
    char *prefix;
    int sent;
    int received;
    struct timeval start;
    double min;
    double max;
    double tsum;
    double tsum2;
};

struct sigaction osa;
struct ccn_ping_statistics sta;

static void ccn_ping_gettime(const struct ccn_gettime *self, struct ccn_timeval *result)
{
    struct timeval now = {0};
    gettimeofday(&now, 0);
    result->s = now.tv_sec;
    result->micros = now.tv_usec;
}

static struct ccn_gettime ccn_ping_ticker = {
    "timer",
    &ccn_ping_gettime,
    1000000,
    NULL
};

static void usage(const char *progname)
{
    fprintf(stderr,
            "Usage: %s ccnx:/name/prefix [options]\n"
            "Ping a CCN name prefix using Interests with name ccnx:/name/prefix/ping/number.\n"
            "The numbers in the Interests are randomly generated unless specified.\n"
            "  [-i interval] - set ping interval in seconds (minimum %.2f second)\n"
            "  [-c count] - set total number of pings\n"
            "  [-n number] - set the starting number, the number is increamented by 1 after each Interest\n"
            "  [-h] - print this message and exit\n",
            progname, PING_MIN_INTERVAL);
    exit(1);
}

static struct ccn_ping_entry *get_ccn_ping_entry(struct ccn_ping_client *client,
        const unsigned char *interest_msg, const struct ccn_parsed_interest *pi)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct ccn_ping_entry *entry;
    int res;

    hashtb_start(client->ccn_ping_table, e);

    res = hashtb_seek(e, interest_msg + pi->offset[CCN_PI_B_Component0],
            pi->offset[CCN_PI_E_LastPrefixComponent] - pi->offset[CCN_PI_B_Component0], 0);

    assert(res == HT_OLD_ENTRY);

    entry = e->data;
    hashtb_end(e);

    return entry;
}

static void remove_ccn_ping_entry(struct ccn_ping_client *client,
        const unsigned char *interest_msg, const struct ccn_parsed_interest *pi)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    int res;

    hashtb_start(client->ccn_ping_table, e);

    res = hashtb_seek(e, interest_msg + pi->offset[CCN_PI_B_Component0],
            pi->offset[CCN_PI_E_LastPrefixComponent] - pi->offset[CCN_PI_B_Component0], 0);

    assert(res == HT_OLD_ENTRY);
    hashtb_delete(e);

    hashtb_end(e);
}

static void add_ccn_ping_entry(struct ccn_ping_client *client,
        struct ccn_charbuf *name, long int number)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct ccn_ping_entry *entry;
    int res;

    hashtb_start(client->ccn_ping_table, e);

    res = hashtb_seek(e, name->buf + 1, name->length - 2, 0);
    assert(res == HT_NEW_ENTRY);

    entry = e->data;
    entry->number = number;
    gettimeofday(&entry->send_time, NULL);

    hashtb_end(e);
}

static enum ccn_upcall_res incoming_content(struct ccn_closure* selfp,
        enum ccn_upcall_kind kind, struct ccn_upcall_info* info)
{
    struct ccn_ping_client *client = selfp->data;
    struct ccn_ping_entry *entry;
    double rtt;
    struct timeval now;

    assert(client->closure == selfp);
    gettimeofday(&now, NULL);

    switch(kind) {
        case CCN_UPCALL_FINAL:
            break;
        case CCN_UPCALL_CONTENT:
            client->received ++;
            sta.received ++;

            entry = get_ccn_ping_entry(client,
                    info->interest_ccnb, info->pi);

            rtt = (double)(now.tv_sec - entry->send_time.tv_sec) * 1000 +
                (double)(now.tv_usec - entry->send_time.tv_usec) / 1000;

            if (rtt < sta.min)
                sta.min = rtt;
            if (rtt > sta.max)
                sta.max = rtt;
            sta.tsum += rtt;
            sta.tsum2 += rtt * rtt;

            printf("content from %s: number = %ld %2s\trtt = %.3f ms\n", client->original_prefix,
                    entry->number, "", rtt);

            remove_ccn_ping_entry(client, info->interest_ccnb, info->pi);

            break;
        case CCN_UPCALL_INTEREST_TIMED_OUT:
            entry = get_ccn_ping_entry(client,
                    info->interest_ccnb, info->pi);

            printf("timeout from %s: number = %ld\n", client->original_prefix, entry->number);

            remove_ccn_ping_entry(client, info->interest_ccnb, info->pi);

            break;
        default:
            fprintf(stderr, "Unexpected response of kind %d\n", kind);
            return CCN_UPCALL_RESULT_ERR;
    }

    return CCN_UPCALL_RESULT_OK;
}

static int do_ping(struct ccn_schedule *sched, void *clienth,
        struct ccn_scheduled_event *ev, int flags)
{
    struct ccn_ping_client *client = clienth;
    if (client->total >= 0 && client->sent >= client->total)
        return 0;

    struct ccn_charbuf *name = ccn_charbuf_create();
    long int rnum;
    char rnumstr[20];
    int res;

    ccn_charbuf_append(name, client->prefix->buf, client->prefix->length);
    if (client->number < 0)
        rnum = random();
    else {
        rnum = client->number;
        client->number ++;
    }
    memset(&rnumstr, 0, 20);
    sprintf(rnumstr, "%ld", rnum);
    ccn_name_append_str(name, rnumstr);

    res = ccn_express_interest(client->h, name, client->closure, NULL);
    add_ccn_ping_entry(client, name, rnum);
    client->sent ++;
    sta.sent ++;

    ccn_charbuf_destroy(&name);

    if (res >= 0)
        return client->interval * 1000000;
    else
        return 0;
}

void print_statistics(void)
{
    printf("\n--- %s ccnping statistics ---\n", sta.prefix);

    if (sta.sent > 0) {
        double lost = (double)(sta.sent - sta.received) * 100 / sta.sent;
        struct timeval now = {0};
        gettimeofday(&now, NULL);
        int time = (double)(now.tv_sec - sta.start.tv_sec) * 1000 +
            (double)(now.tv_usec - sta.start.tv_usec) / 1000;

        printf("%d Interests transmitted, %d Data received, %.1f%% packet loss, time %d ms\n", sta.sent, sta.received, lost, time);
    }

    if (sta.received > 0) {
        double avg = sta.tsum / sta.received;
        double mdev = sqrt(sta.tsum2 / sta.received - avg * avg);
        printf("rtt min/avg/max/mdev = %.3f/%.3f/%.3f/%.3f ms\n", sta.min, avg, sta.max, mdev);
    }
}

void handle_interrupt(int sig_no)
{
    print_statistics();
    sigaction(SIGINT, &osa, NULL);
    kill(0, SIGINT);
}

int main(int argc, char *argv[])
{
    const char *progname = argv[0];
    struct ccn_ping_client client = {.sent = 0, .received = 0, .total = -1, .number = -1, .interval = 1};
    struct ccn_closure in_content = {.p = &incoming_content};
    struct hashtb_param param = {0};
    int res;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &handle_interrupt;
    sigaction(SIGINT, &sa, &osa);

    memset(&sta, 0, sizeof(sta));
    gettimeofday(&sta.start, 0);
    sta.min = INT_MAX;

    while ((res = getopt(argc, argv, "hi:c:n:")) != -1) {
        switch (res) {
            case 'c':
                client.total = atol(optarg);
                if (client.total <= 0)
                    usage(progname);
                break;
            case 'i':
                client.interval = atof(optarg);
                if (client.interval < PING_MIN_INTERVAL)
                    usage(progname);
                break;
            case 'n':
                client.number = atol(optarg);
                if (client.number < 0)
                    usage(progname);
                break;
            case 'h':
            default:
                usage(progname);
                break;
        }
    }

    if (client.number < 0)
        srandom(time(NULL)  * getpid());

    argc -= optind;
    argv += optind;

    if (argv[0] == NULL)
        usage(progname);

    sta.prefix = argv[0];

    client.original_prefix = argv[0];
    client.prefix = ccn_charbuf_create();
    res = ccn_name_from_uri(client.prefix, argv[0]);
    if (res < 0) {
        fprintf(stderr, "%s: bad ccn URI: %s\n", progname, argv[0]);
        exit(1);
    }
    if (argv[1] != NULL)
        fprintf(stderr, "%s warning: extra arguments ignored\n", progname);

    //append "/ping" to the given name prefix
    res = ccn_name_append_str(client.prefix, PING_COMPONENT);
    if (res < 0) {
        fprintf(stderr, "%s: error constructing ccn URI: %s/%s\n", progname, argv[0], PING_COMPONENT);
        exit(1);
    }

    /* Connect to ccnd */
    client.h = ccn_create();
    if (ccn_connect(client.h, NULL) == -1) {
        perror("Could not connect to ccnd");
        exit(1);
    }

    client.closure = &in_content;
    in_content.data = &client;

    client.ccn_ping_table = hashtb_create(sizeof(struct ccn_ping_entry), &param);

    client.sched = ccn_schedule_create(&client, &ccn_ping_ticker);
    client.event = ccn_schedule_event(client.sched, 0, &do_ping, NULL, 0);

    printf("CCNPING %s\n", client.original_prefix);

    res = 0;

    while (res >= 0 && (client.total <= 0 || client.sent < client.total || hashtb_n(client.ccn_ping_table) > 0))
    {
        if (client.total <= 0 || client.sent < client.total)
            ccn_schedule_run(client.sched);
        res = ccn_run(client.h, 10);
    }

    ccn_schedule_destroy(&client.sched);
    ccn_destroy(&client.h);
    ccn_charbuf_destroy(&client.prefix);

    print_statistics();

    return 0;
}
