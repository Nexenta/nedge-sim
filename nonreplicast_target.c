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
    tcp_reception_complete_t *pending;  // conceptual head of pend_completions
                                        // but currently in the event list
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
    unsigned n;
    nrt = (nonrep_target_t *)calloc(n_targets,sizeof(nonrep_target_t));
    
    for (n = 0;n != n_targets;++n) {
        nrt[n].pend_completions.event.tllist.next =
            &nrt[n].pend_completions.event.tllist;
        nrt[n].pend_completions.event.tllist.prev =
            &nrt[n].pend_completions.event.tllist;
    }
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

static void move_first_event_back_to_pending_list (nonrep_target_t *t)
{
    tcp_reception_complete_t *pend;
    
    assert(t);
    pend = t->pending;
    assert(pend);
    tllist_remove(&pend->event.tllist);
    tllist_insert(&t->pend_completions.event.tllist,&pend->event.tllist);
    
    t->pending = (tcp_reception_complete_t *)0;;
}

static void move_first_pending_completion_to_event_list (nonrep_target_t *t)
{
    tcp_reception_complete_t *pend;
    
    pend = (tcp_reception_complete_t *)t->pend_completions.event.tllist.next;
    tllist_remove(&pend->event.tllist);
    __insert_event(&pend->event);

    t->pending = pend;
}

void handle_tcp_xmit_received (const event_t *e)

{
    const tcp_xmit_received_t *txr = (const tcp_xmit_received_t *)e;
    tcp_reception_complete_t *trc = calloc(1,sizeof *trc);
    tcp_reception_ack_t tra;
    const tllist_t *insert_point;
    nonrep_target_t *t;
    
    assert (e);
    assert (trc);
    t = nrt + txr->target_num;

    trc->event.create_time = e->tllist.time;
    trc->event.tllist.time =
        e->tllist.time + derived.chunk_xmit_duration * t->n_pend_completions;
        // TODO: TCP overhead is slightly more per KB
 
    if (t->pending)
        move_first_event_back_to_pending_list(t);
    if (t->n_pend_completions) {
        credit_ongoing_receptions(t,e->tllist.time);
        move_first_pending_completion_to_event_list (t);
    }
    trc->event.type = TCP_RECEPTION_COMPLETE;
    trc->cp = txr->cp;
    trc->target_num = txr->target_num;
    insert_point = tllist_find(&t->pend_completions.event.tllist,trc->event.tllist.time);
    tllist_insert((tllist_t *)insert_point,&trc->event.tllist);
    ++t->n_pend_completions;
    
    tra.event.create_time = e->tllist.time;
    tra.event.tllist.time = e->tllist.time + CLUSTER_TRIP_TIME;
    tra.event.type = TCP_RECEPTION_ACK;
    tra.cp = txr->cp;
    tra.target_num = txr->target_num;
    insert_event(tra);
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
    move_first_pending_completion_to_event_list (t);
    insert_event(dwc);
}
