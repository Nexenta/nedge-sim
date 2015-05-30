//
//  common_target.c
//  StorageClusterSim
//
//  Created by cait on 5/13/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#include "storage_cluster_sim.h"
#include "target.h"

target_t *t;

void init_targets(unsigned n_targets)
//
// Initialize the target subsystem, specifically the irhead for each
// entry in the target_t array 't' must be set to empty.
// Setting the ir_head to point to itself makes an empty list.
//
{
    unsigned n;
    
    t = (target_t *)calloc(n_targets,sizeof(target_t));
    assert(t);
    
    for (n=0;n != n_targets;++n)
        t[n].ir_head.tllist.next = t[n].ir_head.tllist.prev =
        &t[n].ir_head.tllist;
}

void release_targets (void)
{
    free(t);
    t = (target_t *)0;
}

void handle_disk_write_completion (const event_t *e)
//
// Upon disk write completion we schedule transmission of an ack to the gateway
// This work COULD have been done in replicast_target.c and consisthash_target.c
// but having this event in common_target.c helps clarify how the flow of events
// merge after Replicast vs. Non-Replicast handling on the target
//
{
    disk_write_completion_t *wc = (disk_write_completion_t *)e;
    replica_put_ack_t new_event;
    assert(e);
    new_event.event.create_time = e->tllist.time;
    new_event.event.tllist.time = e->tllist.time + CLUSTER_TRIP_TIME;
    new_event.event.type = REPLICA_PUT_ACK;
    new_event.cp = wc->cp;
    assert(chunk_seq(wc->cp));
    new_event.target_num = wc->target_num;
    assert(wc->target_num < derived.n_targets);
    
    insert_event (new_event);
}


