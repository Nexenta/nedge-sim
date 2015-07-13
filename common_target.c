//
//  common_target.c
//  StorageClusterSim
//
//  Created by Caitlin Bestler on 7/9/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#include "storage_cluster_sim.h"

void handle_disk_write_start (const event_t *e)

{
    disk_write_start_t *dws = (disk_write_start_t *)e;
    disk_write_completion_t dwc;
    
    assert(dws->write_qdepth >= 0);
    assert(dws->write_qdepth < 999);
    dwc.event.create_time = e->tllist.time;
    dwc.event.tllist.time = dwc.event.create_time +
                            derived.chunk_disk_write_duration;

    dwc.event.type = DISK_WRITE_COMPLETION;
    dwc.cp = dws->cp;
    dwc.target_num = dws->target_num;
    dwc.write_qdepth = dws->write_qdepth;
    dwc.qptr = dws->qptr;
    ++track.n_writes_jnitiated;
    insert_event(dwc);
}

void handle_disk_write_completion (const event_t *e)
//
// Upon disk write completion we schedule transmission of an ack to the gateway
// This work COULD have been done in replicast_target.c and consisthash_target.c
// but having this event in common_target.c helps clarify how the flow of events
// merge after Replicast vs. Non-Replicast handling on the target
//
{
    disk_write_completion_t *dwc = (disk_write_completion_t *)e;
    char *tag = replicast ? "replicast" : "non";
    replica_put_ack_t rpa;
    assert(e);
    assert(chunk_seq(dwc->cp));
    assert(dwc->write_qdepth >= 0);

    rpa.event.create_time = e->tllist.time;
    rpa.event.tllist.time = e->tllist.time + config.cluster_trip_time;
    rpa.event.type = REPLICA_PUT_ACK;
    rpa.cp = dwc->cp;
    assert(dwc->qptr);
    assert(*dwc->qptr);
    if (!track.drain) {
        if (!config.terse) {
            fprintf(log_f,
                    "%s,DiskWriteCompletion,cp,0x%lx,%d,target,%d,qdepth,%d",
                    tag,rpa.cp,chunk_seq(rpa.cp),dwc->target_num,*dwc->qptr);
            fprintf(log_f,",write_q_depth,%d",dwc->write_qdepth);
            fprintf(log_f,",active_targets,%d\n",track.n_active_targets);
        }
        if (--*dwc->qptr == 0) {
            assert(track.n_active_targets);
            --track.n_active_targets;
        }
    }
    rpa.target_num = dwc->target_num;
    assert(dwc->target_num < derived.n_targets);
    rpa.write_qdepth = dwc->write_qdepth;
    ++track.n_writes_completed;
    insert_event (rpa);
}


