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
    tick_t  credit;
    tick_t  credited_thru;
    chunk_put_handle_t cp;
} ongoing_reception_t;

typedef struct nonrep_target_t {
    target_t    common;
    unsigned n_ongoing_receptions;
    ongoing_reception_t orhead;
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
    assert(!nrt);
    
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
    assert(nrt);
    
    free(nrt);
    nrt = (nonrep_target_t *)0;
}

static void credit_ongoing_receptions (nonrep_target_t *t,tick_t time_now)
{
    tllist_t *pnd;
    ongoing_reception_t *ort;
    tick_t  elapsed_time,credit;
    
    assert (t);
    
    for (pnd = t->orhead.tllist.next;
         pnd != &t->orhead.tllist;
         pnd = pnd->next)
    {
        ort = (ongoing_reception_t *)pnd;
        
        elapsed_time = time_now - ort->credited_thru;
        ort->credited_thru = time_now;
        credit = elapsed_time/t->n_ongoing_receptions;
        if (!credit) credit = 1;
        ort->credit += credit;
    }
}

static void schedule_tcp_reception_complete (
                                             nonrep_target_t *t,
                                             chunk_put_handle_t cp
                                            )
{
    tcp_reception_complete_t trc;
    ongoing_reception_t *ort;
    tick_t remaining_xfer;
    
    assert(t);
    ort = (ongoing_reception_t *)t->orhead.tllist.next;
    assert(ort  &&  &ort->tllist != &t->orhead.tllist);
    
    trc.event.create_time = now;
    remaining_xfer = derived.chunk_xmit_duration - ort->credit;
    trc.event.tllist.time = now + remaining_xfer*t->n_ongoing_receptions;
    trc.event.type = TCP_RECEPTION_COMPLETE;
    trc.cp = cp;
    assert (nrt <= t  &&  t < &nrt[derived.n_targets]);
    trc.target_num = (unsigned)(t - nrt);
    if (t->n_ongoing_receptions > 1) {
        printf("\nNow 0x%lx Target %d # ongoing receptions %d\n",now,
               trc.target_num,t->n_ongoing_receptions);
        for (ort = (ongoing_reception_t *)t->orhead.tllist.next;
             &ort->tllist != &t->orhead.tllist;
             ort = (ongoing_reception_t *)ort->tllist.next)
        {
            printf("CP %d at 0x%lx with credit 0x%lx\n",chunk_seq(ort->cp),
                   ort->tllist.time,ort->credit);
        }
        printf("Adding TRC Reception Complete at 0x%lx for target %d cp %d\n",
               trc.event.tllist.time,trc.target_num,chunk_seq(cp));
    }
    insert_event(trc);
}

void handle_tcp_xmit_received (const event_t *e)

// A new tcp chunk transfer to target_num is beginning 'now'
// We will simulate a miraculous TCP congestion algorithm which *instantly*
// adjust all N flows to be even rate. This is an ideal that can never be
// achieved, but is clearly the goal of all TCP congestion control algorihtms.
//
// The steps
//      Credit existing flows through the present.
//      create the new flow
//      if this was the first flow then schedule the tcp_reception_complete
//      event, otherwise just allow the existing event to complete. This
//      will be slightly early (because there are now n+1 flows instead of
//      n flows, so when that event triggers only (n-1)/n of the payload
//      would have been transferred. We'll just update the credits and
//      re-issue the new tcp_reception_complete event

{
    const tcp_xmit_received_t *txr = (const tcp_xmit_received_t *)e;
    ongoing_reception_t *ort = calloc(1,sizeof *ort);
    tllist_t *insert_point;
 
    nonrep_target_t *t;
    
    assert (e);
    assert(!replicast);
    t = nrt + txr->target_num;

    if (t->n_ongoing_receptions)
        credit_ongoing_receptions(t,e->tllist.time);
   
    assert(ort);
    ort->tllist.time = e->tllist.time;
    ort->credited_thru = e->tllist.time;
    ort->cp = txr->cp;
    insert_point = (tllist_t *)tllist_find(&t->orhead.tllist,ort->tllist.time);
    tllist_insert(insert_point,&ort->tllist);

    if (++t->n_ongoing_receptions == 1)
        schedule_tcp_reception_complete (t,txr->cp);
}

void handle_tcp_reception_complete (const event_t *e)

// handle the expected completion of a TCP chunk reception.
//
// while there is an ongoing reception queue (orhead) for the target
//      if the lead ongoing reception does not have enough credit yet
//          schedule the next tcp_reception_complete event
//          break
//      Make a Disk Write Completion event for the completed ongoing reception
//      consume the ongoing reception

{
    const tcp_reception_complete_t *trc = (const tcp_reception_complete_t *)e;
    ongoing_reception_t *ort,*ort_next;
    disk_write_completion_t dwc;
    nonrep_target_t *t;
    tick_t write_start;

    assert (e); (void)e;
    assert(!replicast);
    t = nrt + trc->target_num;
    
    assert(t->n_ongoing_receptions);
    credit_ongoing_receptions(t,e->tllist.time);

    for (ort = (ongoing_reception_t *)t->orhead.tllist.next;
         &ort->tllist != &t->orhead.tllist;
         ort = ort_next)
    {
        ort = (ongoing_reception_t *)t->orhead.tllist.next;
        if (ort->credit <derived.chunk_xmit_duration) {
            schedule_tcp_reception_complete (t,trc->cp);
            break;
        }
        dwc.event.create_time = e->tllist.time;
        write_start = (t->last_disk_write_completion > e->tllist.   time)
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
        
        ort_next = (ongoing_reception_t *)ort->tllist.next;
        tllist_remove(&ort->tllist);
        free(ort);
    }
}
