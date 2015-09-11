//
//  main.c
//  CongestionSim
//
//  Created by Caitlin Bestler on 9/3/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "StorageSim.h"
#include "network_sim.h"

static tllist_t events;

static tllist_t *tllist_remove (tllist_t *head)
{
    tllist_t *p;
    
    assert (head);
    p = head->next;
    if (p == head)
        p = NULL;
    else {
        head->next = p->next;
        p->next->prev = head;
    }
    return p;
}


bool running = true; // TODO: set to FALSE wjhen simulation done
                        // when false do not initiate new chunk puts

// TODO add seed events that trigger successsor seed events on each node
// give an option to control which nodes generate transactions,
//      model one: some nodes are clients/gateways, others servers
//      model two: all are both.
//      model three: all are both, but some are mostly gateways, other servers.

static void simulate (void)
{
    output_queue_t *from_q;
    input_queue_t * to_q;
    union event_union {
        event_t *e;
        ding_event_t *de;
        msg_xfer_start_event_t *mse;
        msg_xfer_done_event_t *mde;
        initiate_event_t *ie;
    } eu;
    message_t *m;
    packet_t *p;
    placed_packet_t *pp;
    node_t *node;
    tick_t when;
    
    while ((eu.e = (event_t *)tllist_remove(events.next)) != NULL) {
        switch (eu.e->event_type) {
            case INITIATE_PUT:
                if (running) {
                    node = eu.ie->node;
                    // determine delay based on congestion history
                    // build CHUNK_PUT message m with 1 packet p
                    enqueue_packet(when,true,p,&node->output);
                }
                break;
            case CONGESTION_DING:
                // extend/create transmission break from this node to destination
                // unlike DCB QCN, however, this break applies only to initial
                // unsolicited commands.
                break;
            case MSG_XFER_START:
                from_q = &eu.mse->from_node->output;
                to_q = &eu.mse->to_node->input;
                ++to_q->packet_depth;
                to_q->byte_depth  += eu.mse->pkt->packet_size;
                //      ding source if congested
                p = eu.mse->pkt;
                pp = (placed_packet_t *)from_q->queue.packet_queue_head.next;
                if (p == pp->packet) {
                    // for each destination
                            destination_enqueue(p,&eu.mse->to_node->input);
                    //      build MSG_XFER_DONE event for pkt/source/destination
                    //          allowing for xmit time for packet_size bytes
                    //      insert_event
                }
                break;
            case MSG_XFER_DONE:
                from_q = &eu.mde->from_node->output;
                to_q = &eu.mde->to_node->input;
                tllist_remove(&from_q->queue.packet_queue_head);
                // TODO: release pp, but when to release pkt/msg?
                pp = (placed_packet_t *)
                    tllist_remove(&to_q->queue.packet_queue_head);

                node = (node_t *)eu.mde->to_node; // TODO: fix const
                process(node,pp->packet);
                // reduce reference count on pkt, if now zero
                //      reduce reference count on msg, if now zero free msg
                //      free pkt
                free(pp);
                // if msg complete at source and there is another packet
                //    ready to go in the source queue then
                //      build MSG_XFER_START event
                //      insert event
                break;
        }
        free(eu.e);
    }
}

node_t *nodes;

static void init_tllist (tllist_t *t)
{
    t->next = t->prev = t;
}

static void init_nodes (void)
{
    node_t *n,*lim;
    
    nodes = calloc(config.n_nodes,sizeof *nodes);
    assert(nodes);
    for (n = nodes,lim = n+config.n_nodes; n != lim; ++n) {
        n->output.queue.node = n;
        init_tllist(&n->output.queue.packet_queue_head);
        n->input.queue.node = n;
        init_tllist(&n->input.queue.packet_queue_head);
    }
}

config_t config = {
    .n_nodes = 1000,
    .per_packet_overhead = 70,
    .cluster_trip_time = 2000
};

tick_t now = 0L;

static void customize_config (int argc,const char *argv[])
{
    
}

int main(int argc, const char * argv[]) {
    customize_config(argc,argv);
    init_nodes();
    simulate();
    free(nodes);
    return 0;
}
