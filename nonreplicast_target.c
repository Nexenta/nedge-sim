//
//  nonreplicast_target.c
//  StorageClusterSim
//
//  Created by cait on 5/26/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#include "storage_cluster_sim.h"

typedef struct ongoing_reception {
    tllist_t    tllist; // time is time tcp reception started
    tick_t  credited_thru;
    tick_t  credit;
    chunk_put_handle_t cp;
} ongoing_reception_t;

typedef struct nonrep_target_t {
    target_t    common;
    unsigned n_ongoing_receptions;
    ongoing_reception_t orhead;
    unsigned n_pend_completions;
    tick_t last_disk_write_completion;
    unsigned mbz;
} nonrep_target_t;
//
// A struct target represents the target specific data that each individual
// target would have stored separately. This includes the queue of inbound
// reservations and when the last disk write completion would have occurred.
//

static nonrep_target_t *nrt = NULL;

void init_nonrep_targets(unsigned n_targets)
//
// Initialize the target subsystem, specifically the irhead for each
// entry in the target_t array 't' must be set to empty.
// Setting the ir_head to point to itself makes an empty list.
//
{
    unsigned n;
    
    assert(!replicast);
    
    nrt = (nonrep_target_t *)calloc(n_targets,sizeof(nonrep_target_t));
 
    assert(nrt);
    for (n = 0;n != n_targets;++n) {
        nrt[n].orhead.tllist.next = nrt[n].orhead.tllist.prev =
            &nrt[n].orhead.tllist;
    }
}

void release_nonrep_targets (void)
{
    assert(!replicast);
    free(nrt);
    nrt = (nonrep_target_t *)0;
}

static void credit_ongoing_receptions (nonrep_target_t *t,tick_t time_now)
{
    tllist_t *pnd;
    ongoing_reception_t *ort;
    
    assert (t);
    (void)ort;
//    elapsed_time = time_now - t->prior_reception_time;
//    t->prior_reception_time = time_now;
//    credit = elapsed_time/(t->n_pend_completions+1);
    
    for (pnd = t->orhead.tllist.next;
         pnd != &t->orhead.tllist;
         pnd = pnd->next)
    {
        ort = (ongoing_reception_t *)pnd;
        
 //       trc->credit += credit;
    }
}

void handle_tcp_xmit_received (const event_t *e)

{
    const tcp_xmit_received_t *txr = (const tcp_xmit_received_t *)e;
    ongoing_reception_t *ort = calloc(1,sizeof *ort);
    tcp_reception_complete_t *trc = calloc(1,sizeof *trc);
    nonrep_target_t *t;
    
    assert (e);
    assert (trc);
    assert(!replicast);
    t = nrt + txr->target_num; (void)t;

    // give credit to existing ongoing_receptions
    
    ort->tllist.time = e->tllist.time;
    ort->credited_thru = e->tllist.time;
    // insert ort in t->orhead
    
    // if this was the first then schedule a tcp_reception_complete event
}

void handle_tcp_reception_complete (const event_t *e)
{
    const tcp_reception_complete_t *trc = (const tcp_reception_complete_t *)e;
    disk_write_completion_t dwc;
    nonrep_target_t *t;
    tick_t write_start;

    assert (e); (void)e;
    assert(!replicast);
    t = nrt + trc->target_num;
    
    credit_ongoing_receptions(t,e->tllist.time);

    // while head or orhead is done
    dwc.event.create_time = e->tllist.time;
    
    write_start = (t->last_disk_write_completion > e->tllist.time)
        ? t->last_disk_write_completion
        : e->tllist.time;
    dwc.event.create_time = e->tllist.time;
    dwc.event.tllist.time = write_start + derived.chunk_disk_write_duration;
    t->last_disk_write_completion = dwc.event.tllist.time;
    dwc.event.type = DISK_WRITE_COMPLETION;
    dwc.cp = trc->cp;
    dwc.target_num = trc->target_num;
    dwc.write_qdepth = t->common.write_qdepth++;
    dwc.qptr = &t->common.write_qdepth;
    insert_event(dwc);
    
    // if orhead next exists schedule the next tcp_reception_complete event
}
