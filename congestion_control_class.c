//
//  congestion_control_class.c
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


static void congestion_control_process (node_t *node,placed_packet_t *pp)
{
    const packet_t *p;
    const message_t *m;
    queue_t *dest_q;
    tick_t delay;
    tick_t window = config.congestion_xmit_window;
    
    assert (pp);
    p = pp->packet;
    assert(p);
    m = p->message;
    assert(m);
    
    if (m->solicited) {
        dest_q = &node->output_solicited;
        delay = 0L;
    }
    else if (m->message_type != PUT_REQUEST) {
        dest_q = &node->output_unsolicited;
        delay = 0L;
    }
    else {
        dest_q = &node->output_unsolicited;
        // calculate delay for congestion control
        // if traffic tracking data is available
        //      window_multipler = estimated_traffic / quota traffic
        //      window = quote_window * window_multiplier
        // else only congestion tracking data is available
        //      adjust window back to quota_xmit_window based on time since last
        delay = rand() % window;
    }
    enqueue_packet (now+delay,p,dest_q);
}


queue_class_t congestion_control_class = {
    CONGESTION_CONTROL,
    congestion_control_process
};

