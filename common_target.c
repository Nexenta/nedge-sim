//
//  common_target.c
//  StorageClusterSim
//
//  Created by cait on 5/13/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#include "storage_cluster_sim.h"

void handle_disk_write_completion (const event_t *e)
//
// Upon disk write completion we schedule transmission of an ack to the gateway
// This work COULD have been done in replicast_target.c and consisthash_target.c
// but having this event in common_target.c helps clarify how the flow of events
// merge after Replicast vs. Non-Replicast handling on the target
//
{
    disk_write_completion_t *dwc = (disk_write_completion_t *)e;
    replica_put_ack_t new_event;
    assert(e);

    
    new_event.event.create_time = e->tllist.time;
    new_event.event.tllist.time = e->tllist.time + CLUSTER_TRIP_TIME;
    new_event.event.type = REPLICA_PUT_ACK;
    new_event.cp = dwc->cp;
    assert(dwc->qptr);
    assert(*dwc->qptr);
    --*dwc->qptr;
    
    assert(chunk_seq(dwc->cp));
    
    new_event.target_num = dwc->target_num;
    new_event.write_qdepth = dwc->write_qdepth;
    assert(dwc->target_num < derived.n_targets);
    
    insert_event (new_event);
}


