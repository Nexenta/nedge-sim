//
//  output_queue.c
//  CongestionSim
//
//  Created by cait on 9/8/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//



#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "StorageSim.h"
#include "network_sim.h"


static void output_process (node_t *node,placed_packet_t *pp)
{
    const packet_t *p;
    const message_t *m;
    queue_t *dest_q;
    unsigned i;
    tick_t when;
    
    assert (pp);
    p = pp->packet;
    assert(p);
    m = p->message;
    assert(m);
    
    for (i = 0; i != m->dest.n_destinations; ++i) {
        // determine when
        dest_q = &nodes[m->dest.target[i]].core;
        // how to make a track of dest?
        enqueue_packet(when,dest_q,p);
    }
}


queue_class_t output_unsolicited_class = {
    OUTPUT_UNSOLICITED,
    output_process
};

queue_class_t output_solicited_class = {
    OUTPUT_SOLICITED,
    output_process
};


