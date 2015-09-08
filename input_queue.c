//
//  input_queue.c
//  CongestionSim
//
//  Created by cait on 9/8/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#include <stdio.h>

#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "StorageSim.h"
#include "network_sim.h"

static void input_process (node_t *node,placed_packet_t *pp)
{
    const packet_t *p;
    const message_t *m;
    queue_t *dest_q;
    
    assert (pp);
    p = pp->packet;
    assert(p);
    m = p->message;
    assert(m);
    
    // dest_q is same node as this .process
    enqueue_packet(now,dest_q,p);
}

queue_class_t input_unsolicited_class = {
    INPUT_UNSOLICITED,
    input_process
};

queue_class_t input_solicited_class = {
    INPUT_SOLICITED,
    input_process
};
