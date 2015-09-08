//
//  core.c
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

static void core_process (node_t *node,placed_packet_t *pp)
{
    const packet_t *p;
    const message_t *m;
    queue_t *dest_q;
    tick_t when;
    
    assert (pp);
    p = pp->packet;
    assert(p);
    m = p->message;
    assert(m);
    
    // dest_q is same destination as this queue, .input_[un]solicited
    // determine when
    enqueue_packet(when,dest_q,p);
}

queue_class_t core_class = {
    CORE,
    core_process
};



