//
//  nonreplicast_target.c
//  StorageClusterSim
//
//  Created by cait on 5/26/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#include "storage_cluster_sim.h"

typedef struct nonrep_target_t {
    target_t    common;
    unsigned n_pend_completions;
    tcp_reception_complete_t pend_completions;
    tick_t last_disk_write_completion;
    tick_t prior_reception_time;
    unsigned mbz;
} nonrep_target_t;
//
// A struct target represents the target specific data that each individual
// target would have stored separately. This includes the queue of inbound
// reservations and when the last disk write completion would have occurred.
//

static nonrep_target_t *nrt;

void init_nonrep_targets(unsigned n_targets)
//
// Initialize the target subsystem, specifically the irhead for each
// entry in the target_t array 't' must be set to empty.
// Setting the ir_head to point to itself makes an empty list.
//
{ 
    nrt = (nonrep_target_t *)calloc(n_targets,sizeof(nonrep_target_t));
    assert(nrt);
}

void release_nonrep_targets (void)
{
    free(nrt);
    nrt = (nonrep_target_t *)0;
}

static void credit_ongoing_receptions (nonrep_target_t *t,tick_t time_now)
{
    tllist_t *pnd;
    tcp_reception_complete_t *trc;
    tick_t elapsed_time,credit;
    
    assert (t);
    elapsed_time = time_now - t->prior_reception_time;
    t->prior_reception_time = time_now;
    credit = elapsed_time/t->n_pend_completions;
    
    
    for (pnd = t->pend_completions.event.tllist.next;
         pnd != &t->pend_completions.event.tllist;
         pnd = pnd->next)
    {
        trc = (tcp_reception_complete_t *)pnd;
        
        trc->credit += credit;
    }
    
    
}

void handle_tcp_xmit_received (const event_t *e)

{

    const tcp_xmit_received_t *txr = (const tcp_xmit_received_t *)e;
    tcp_reception_complete_t trc;
    nonrep_target_t *t;
    
    assert (e);
    t = nrt + txr->target_num;
    ++t->n_pend_completions;
    trc.event.create_time = e->tllist.time;
    trc.event.tllist.time =
        e->tllist.time + derived.chunk_xmit_duration * t->n_pend_completions;
        // TODO: TCP overhead is slightly more per KB
    
    credit_ongoing_receptions(t,e->tllist.time);

    trc.event.type = TCP_RECEPTION_COMPLETE;
    trc.cp = txr->cp;
    trc.target_num = txr->target_num;
    // insert trc in t->pend_completions
    //
    // if lead event changed then remove it from event list and re-insert
    // at its new time (but do not free anything
    //
    assert(false);  // TODO: fixme
}

void handle_tcp_reception_complete (const event_t *e)
{
    const tcp_reception_complete_t *trc = (const tcp_reception_complete_t *)e;
    disk_write_completion_t dwc;
    nonrep_target_t *t;
    tick_t write_start;

    assert (e); (void)e;
    t = nrt + trc->target_num;
    
    credit_ongoing_receptions(t,e->tllist.time);
    

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
    //  remove head of target's pend receptions
    //  insert it as an event
    insert_event(dwc);
    
    assert(false);  // TODO: fixme
}
