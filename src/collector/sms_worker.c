/*
 *
 * Copyright (c) 2018-2023 Searchlight New Zealand Ltd.
 * All rights reserved.
 *
 * This file is part of OpenLI.
 *
 * OpenLI was originally developed by the University of Waikato WAND
 * research group. For further information please see http://www.wand.net.nz/
 *
 * OpenLI is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenLI is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */

#include "netcomms.h"
#include "intercept.h"
#include "collector.h"
#include "sms_worker.h"
#include "util.h"
#include "logger.h"

#include <sys/timerfd.h>
#include <libtrace.h>

static int sms_worker_process_packet(openli_sms_worker_t *state) {

    openli_state_update_t recvd;
    int rc;

    do {
        rc = zmq_recv(state->zmq_colthread_recvsock, &recvd, sizeof(recvd),
                ZMQ_DONTWAIT);
        if (rc < 0) {
            if (errno == EAGAIN) {
                return 0;
            }
            logger(LOG_INFO,
                    "OpenLI: error while receiving packet in SMS worker thread %d: %s",
                    state->workerid, strerror(errno));
            return -1;
        }

        /* TODO
         *
         * is it a "MESSAGE" ?
         *   is it TO or FROM an intercept target?
         *      is it a new call-ID or an existing one?
         *         new: create "session"
         *         existing: grab CIN etc. from existing session
         *      create an IRI from this packet
         *
         * else: does the call ID match a known intercept session?
         *      create an IRI from this packet
         *
         * if intercepted, update timestamp of last session activity
         */

         trace_destroy_packet(recvd.data.pkt);
    } while (rc > 0);
    return 0;
}

static int sms_worker_handle_provisioner_message(openli_sms_worker_t *state,
        openli_export_recv_t *msg) {

    int ret = 0;
    switch(msg->data.provmsg.msgtype) {
        case OPENLI_PROTO_START_VOIPINTERCEPT:
            break;
        case OPENLI_PROTO_HALT_VOIPINTERCEPT:
            break;
        case OPENLI_PROTO_MODIFY_VOIPINTERCEPT:
            break;
        case OPENLI_PROTO_ANNOUNCE_SIP_TARGET:
            break;
        case OPENLI_PROTO_WITHDRAW_SIP_TARGET:
            break;
        case OPENLI_PROTO_NOMORE_INTERCEPTS:
            //sms_worker_disable_unconfirmed_voip_intercepts(state);
            break;
        case OPENLI_PROTO_DISCONNECT:
            //sms_worker_flag_all_intercepts(state);
            break;
        default:
            logger(LOG_INFO, "OpenLI: SMS worker thread %d received unexpected message type from provisioner: %u",
                    state->workerid, msg->data.provmsg.msgtype);
            ret = -1;
    }

    if (msg->data.provmsg.msgbody) {
        free(msg->data.provmsg.msgbody);
    }

    return ret;
}


static int sms_worker_process_sync_thread_message(openli_sms_worker_t *state) {

    openli_export_recv_t *msg;
    int x;

    do {
        x = zmq_recv(state->zmq_ii_sock, &msg, sizeof(msg), ZMQ_DONTWAIT);
        if (x < 0 && errno != EAGAIN) {
            logger(LOG_INFO,
                    "OpenLI: error while receiving II in SMS thread %d: %s",
                    state->workerid, strerror(errno));
            return -1;
        }

        if (x <= 0) {
            break;
        }

        if (msg->type == OPENLI_EXPORT_HALT) {
            free(msg);
            return -1;
        }

        if (msg->type == OPENLI_EXPORT_PROVISIONER_MESSAGE) {
            if (sms_worker_handle_provisioner_message(state, msg) < 0) {
                free(msg);
                return -1;
            }
        }

        free(msg);
    } while (x > 0);

    return 1;

}

static void sms_worker_main(openli_sms_worker_t *state) {
    zmq_pollitem_t *topoll;
    sync_epoll_t purgetimer;
    struct itimerspec its;
    int x;

    logger(LOG_INFO, "OpenLI: starting SMS worker thread %s", state->workerid);

    topoll = calloc(3, sizeof(zmq_pollitem_t));

    its.it_value.tv_sec = 60;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;

    purgetimer.fdtype = 0;
    purgetimer.fd = timerfd_create(CLOCK_MONOTONIC, 0);
    timerfd_settime(purgetimer.fd, 0, &its, NULL);

    while (1) {
        topoll[0].socket = state->zmq_ii_sock;
        topoll[0].events = ZMQ_POLLIN;

        topoll[1].socket = state->zmq_colthread_recvsock;
        topoll[1].events = ZMQ_POLLIN;

        topoll[2].socket = NULL;
        topoll[2].fd = purgetimer.fd;
        topoll[2].events = ZMQ_POLLIN;

        if ((x = zmq_poll(topoll, 3, 50)) < 0) {
            if (errno == EINTR) {
                continue;
            }
            logger(LOG_INFO,
                    "OpenLI: error while polling in SMS worker thread %d: %s",
                    state->workerid, strerror(errno));
            break;
        }

        if (x == 0) {
            continue;
        }

        if (topoll[0].revents & ZMQ_POLLIN) {
            /* message from the sync thread */
            x = sms_worker_process_sync_thread_message(state);
            if (x < 0) {
                break;
            }
            topoll[0].revents = 0;
        }

        if (topoll[1].revents & ZMQ_POLLIN) {
            /* a packet passed on from a collector thread */
            x = sms_worker_process_packet(state);
            if (x < 0) {
                break;
            }
            topoll[1].revents = 0;
        }

        if (topoll[2].revents & ZMQ_POLLIN) {
            /* expiry check is due for all known call-ids */
            logger(LOG_INFO, "DEVDEBUG: checking for expired SMS call IDs");
            topoll[2].revents = 0;

            purgetimer.fdtype = 0;
            purgetimer.fd = timerfd_create(CLOCK_MONOTONIC, 0);
            timerfd_settime(purgetimer.fd, 0, &its, NULL);

            topoll[2].fd = purgetimer.fd;
        }
    }

}

void *start_sms_worker_thread(void *arg) {
    openli_sms_worker_t *state = (openli_sms_worker_t *)arg;
    char sockname[256];
    int zero = 0, x;
    openli_state_update_t recvd;

    state->zmq_pubsocks = calloc(state->tracker_threads, sizeof(void *));

    init_zmq_socket_array(state->zmq_pubsocks, state->tracker_threads,
            "inproc://openlipub", state->zmq_ctxt);

    state->zmq_ii_sock = zmq_socket(state->zmq_ctxt, ZMQ_PULL);
    snprintf(sockname, 256, "inproc://openlismscontrol_sync-%d",
            state->workerid);
    if (zmq_bind(state->zmq_ii_sock, sockname) < 0) {
        logger(LOG_INFO, "OpenLI: SMS processing thread %d failed to bind to II zmq: %s", state->workerid, strerror(errno));
        goto haltsmsworker;
    }

    if (zmq_setsockopt(state->zmq_ii_sock, ZMQ_LINGER, &zero, sizeof(zero))
            != 0) {
        logger(LOG_INFO, "OpenLI: SMS processing thread %d failed to configure II zmq: %s", state->workerid, strerror(errno));
        goto haltsmsworker;
    }

    state->zmq_colthread_recvsock = zmq_socket(state->zmq_ctxt, ZMQ_PULL);
    snprintf(sockname, 256, "inproc://openlismsworker-colrecv%d",
            state->workerid);

    if (zmq_bind(state->zmq_colthread_recvsock, sockname) < 0) {
        logger(LOG_INFO, "OpenLI: SMS processing thread %d failed to bind to colthread zmq: %s", state->workerid, strerror(errno));
        goto haltsmsworker;
    }

    if (zmq_setsockopt(state->zmq_colthread_recvsock, ZMQ_LINGER, &zero,
            sizeof(zero)) != 0) {
         logger(LOG_INFO, "OpenLI: SMS processing thread %d failed to configure colthread zmq: %s", state->workerid, strerror(errno));
         goto haltsmsworker;
    }


    sms_worker_main(state);

    do {
        /* drain any remaining captured packets in the recv queue */
        x = zmq_recv(state->zmq_colthread_recvsock, &recvd, sizeof(recvd),
                ZMQ_DONTWAIT);
        if (x > 0) {
            trace_destroy_packet(recvd.data.pkt);
        }
    } while (x > 0);

haltsmsworker:
    logger(LOG_INFO, "OpenLI: halting SMS processing thread %s",
            state->workerid);

    zmq_close(state->zmq_ii_sock);
    zmq_close(state->zmq_colthread_recvsock);

    clear_zmq_socket_array(state->zmq_pubsocks, state->tracker_threads);

    pthread_exit(NULL);
}

// vim: set sw=4 tabstop=4 softtabstop=4 expandtab :
