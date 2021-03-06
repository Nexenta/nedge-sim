//
//  common_target.c
//  StorageClusterSim
//
//  Created by Caitlin Bestler on 7/9/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#include "storage_cluster_sim.h"

typedef struct chunkput_omhucast_t {
    chunkput_t  cp;
    unsigned ch_targets[MAX_REPLICAS]; // selected targets
    unsigned repnum;                    // # of replicas previously generated.
    unsigned acked;
    unsigned max_ongoing_rx;    // maximum n_ongoing_receptions for any target
} chunkput_omhtcp_t;

void handle_disk_write_start (const event_t *e)

{
    disk_write_start_t *dws = (disk_write_start_t *)e;
    disk_write_completion_t dwc;
    
    dwc.event.create_time = e->tllist.time;
    dwc.event.tllist.time = dws->expected_done;

    dwc.event.type = DISK_WRITE_COMPLETION;
    dwc.cp = dws->cp;
    dwc.target_num = dws->target_num;
    ++track.n_writes_initiated;
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
    replica_put_ack_t rpa;
    assert(e);
    assert(chunk_seq(dwc->cp));

    rpa.event.create_time = e->tllist.time;
    rpa.event.tllist.time = e->tllist.time + config.cluster_trip_time;
    rpa.event.type = REPLICA_PUT_ACK;
    rpa.cp = dwc->cp;

    if (!track.drain) {
        if (!config.terse) {
            fprintf(log_f,
                    "%s,DiskWriteCompletion,cp,0x%lx,%d,target,%d,",
                    protocol->tag,rpa.cp,chunk_seq(rpa.cp),dwc->target_num);
            fprintf(log_f,",active_targets,%d\n",track.n_active_targets);
        }
    }
    rpa.target_num = dwc->target_num;
    assert(dwc->target_num < derived.n_targets);
    ++track.n_writes_completed;
    insert_event (rpa);
}

void inc_target_total_queue(unsigned target_num)
{
    target_t *t = protocol->target(target_num);
    
    assert(t);
    ++t->total_inflight;
    if (t->total_inflight == 1) ++track.n_active_targets;
}

void dec_target_total_queue(unsigned target_num)
{
    target_t *t = protocol->target(target_num);
    
    assert(t);
    
    --t->total_inflight;
    if (t->total_inflight == 0) --track.n_active_targets;
}


