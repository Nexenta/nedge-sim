//
//  network_sim.c
//  CongestionSim
//
//  Created by cait on 9/4/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//


#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "StorageSim.h"
#include "network_sim.h"

void enqueue_packet (tick_t when,bool prioritized,const packet_t *p,
                     output_queue_t *q)

// insert a placed_packet referencing 'p' into 'cp' at the end of 'cp's queue
// calculate duration in that queue based on total packet length.

{
    placed_packet_t *pp = calloc(1,sizeof *pp);
    tllist_t *insert_after;
    
    assert(pp);

    if (q->queue.packet_queue_head.next == &q->queue.packet_queue_head)
        insert_after = &q->queue.packet_queue_head;
    else if (prioritized) {
        insert_after = q->queue.packet_queue_head.next;
        // advance through all unsolicited packets
        
    }
    else
        insert_after = q->queue.packet_queue_head.prev;
    pp->tllist.time = when;
    tllist_insert(insert_after,&pp->tllist);
}

void simulate_msg_send (node_t *n,message_t *msg)
{
    output_queue_t *out_q;
    packet_t *p;
    unsigned i,total;
    tick_t delay;
    tick_t when;
    
    assert(n);
    assert(msg);
    assert(msg->max_packet_size);
    assert(msg->message_size);
    assert(msg->dest.n_destinations);
    p = calloc(1,sizeof *p);
    assert(p);
    p->message = msg;
    
    msg->initiated_at = when = now;
    if (msg->message_type == PUT_REQUEST) {
        // calculate delay based on node's ding history
        when = now + delay;
        p->packet_size = msg->message_size + config.per_packet_overhead;
        enqueue_packet(when,!msg->solicited,p,&n->output);
    }
    else {
        out_q = &n->output;
        for (i = total = 0; total < msg->message_size;) {
            p->packet_size = msg->max_packet_size;
            if (p->packet_size > msg->message_size - total)
                p->packet_size = msg->message_size - total;
            total += p->packet_size;
            p->packet_size += config.per_packet_overhead;
            p->packet_num = i;
            enqueue_packet(now,!msg->solicited,p,out_q);
        }
    }
}