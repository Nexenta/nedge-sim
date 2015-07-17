//
//  replicast_target.c
//  StorageClusterSim
//
//  Created by Caitlin Bestler on 7/9/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#include "storage_cluster_sim.h"

static unsigned total_reservations = 0;

typedef struct rep_target_t {       // Track replicast target
    target_t    common;             // common fields
    inbound_reservation_t ir_head;  // tllist of inbound reservations
    int ir_queue_depth;        // # of inbound reservations
    unsigned write_queue_depth;     // depth of write queue for this target
    tick_t last_disk_write_completion;  // last disk write completion for
                                        // this target
    unsigned chunks_put;       // total number of chunks put to this target
    unsigned mbz;   // debugging paranoia
} rep_target_t;
//
// A struct target represents the target specific data that each individual
// target would have stored separately. This includes the queue of inbound
// reservations and when the last disk write completion would have occurred.
//

static rep_target_t *rept = NULL;

void init_rep_targets(unsigned n_targets)
//
// Initialize the target subsystem, specifically the irhead for each
// entry in the target_t array 't' must be set to empty.
// Setting the ir_head to point to itself makes an empty list.
//
{
    unsigned n;
    
    rept = (rep_target_t *)calloc(n_targets,sizeof(rep_target_t));
    assert(rept);
    assert(replicast);
    
    for (n=0;n != n_targets;++n)
        rept[n].ir_head.tllist.next = rept[n].ir_head.tllist.prev =
        &rept[n].ir_head.tllist;
}

void release_rep_targets (void)
{
    free(rept);
    rept = (rep_target_t *)0;
}

#define WRITE_QUEUE_THRESH 10

static void make_bid (unsigned target_num,
                      chunk_put_handle_t cp,
                      tick_t *start,
                      tick_t *lim,
                      tick_t *ack_at,
                      int *qdepth)

// Make a bid to receive 'chunk_num' on 'target_num' later than 'now' lasting
// at least 'duration'. Record this bid as a new inbound_reservation for the
// target and in *start and *lim
//
// Also record an estimate for when this replica would be acked (post write)
//
// duration should already be padded to allow the Gateway to match varying bids
//

{
    inbound_reservation_t *ir =
        (inbound_reservation_t *)calloc(1,sizeof(inbound_reservation_t));
    inbound_reservation_t *p;
    inbound_reservation_t *insert_after;
    rep_target_t *tp = rept + target_num;
    tick_t s;
    tick_t estimated_write_start;

    bool delayed;

    assert(start);
    assert(lim);
    assert(ack_at);
    assert(ir);
    assert(target_num < derived.n_targets);
    assert(replicast);
    
    // make initial guess
    *start = s = now + 2*config.cluster_trip_time +
        config.replicast_packet_processing_penalty;;
    *lim = s +
	(derived.chunk_udp_xmit_duration*config.bid_window_multiplier_pct)/100;
    assert(*start < *lim);
    
    for (p = (inbound_reservation_t *)tp->ir_head.tllist.next, delayed = false;
         p != &tp->ir_head;
         p = (inbound_reservation_t *)p->tllist.next)
    {

        if (p->lim < *start) continue; // is is over before this bid starts
        if (p->tllist.time > *lim) break;  // remaianing reservations are later

        // adjust guess to be after inbound_reservation p
        *start = p->lim + 1;
        *lim = *start +
            (derived.chunk_udp_xmit_duration*config.bid_window_multiplier_pct)
	    / 100;
        delayed = true;
    }
    assert(*start < *lim);
    ++track.n_reservations;
    if (delayed)
        ++track.n_reservation_conflicts;

    estimated_write_start = *lim;
    if (tp->last_disk_write_completion > estimated_write_start)
        estimated_write_start = tp->last_disk_write_completion;
    //
    // This is an *estimated* ack, we don't know the variation yet
    //
    *ack_at = estimated_write_start + config.cluster_trip_time +
        derived.chunk_disk_write_duration;
    
    ir->tllist.time = *start;
    ir->lim = *lim;
    ir->cp = cp;
    assert(chunk_seq(cp));
    
    insert_after =
        (inbound_reservation_t *)tllist_find ((tllist_t *)&tp->ir_head,*start);
    tllist_insert ((tllist_t *)insert_after,(tllist_t *)ir);
    assert(qdepth);
    *qdepth = tp->ir_queue_depth++;
    assert(tp->ir_queue_depth < 999);
    ++total_reservations;
}

void handle_rep_chunk_put_request_received (const event_t *e)

// Generate a Chunk Put Response with a bid for the chunk put
// This involves making a bid which is for this target. It must:
//      Tentatively reserve 3x the transmission time required.
//      Not conflict with any existing inbound_reservation for this target

{
    const rep_chunk_put_request_received_t *cpr =
        (const rep_chunk_put_request_received_t *)e;
    rep_chunk_put_response_received_t cpresp;
    
    assert(e);
    assert(replicast);
    
    cpresp.event.create_time = e->tllist.time;
    cpresp.event.tllist.time = e->tllist.time + config.cluster_trip_time +
                                config.replicast_packet_processing_penalty;
    cpresp.event.type = REP_CHUNK_PUT_RESPONSE_RECEIVED;
    cpresp.cp = cpr->cp;
    cpresp.target_num = cpr->target_num;
    make_bid(cpresp.target_num,cpresp.cp,&cpresp.bid_start,&cpresp.bid_lim,
             &cpresp.estimated_ack,&cpresp.qdepth);
    insert_event(cpresp);
}

static inbound_reservation_t *ir_find_by_cp (
        rep_target_t *tp,
        chunk_put_handle_t cp
)
//
// find inbound reservation for 'cp' on target-specific linked list
//
{
    inbound_reservation_t *ir;
    assert(tp);
    assert(tp->ir_head.tllist.next);
    
    for(ir = (inbound_reservation_t *)tp->ir_head.tllist.next;
        ir != &tp->ir_head;
        ir = (inbound_reservation_t *)ir->tllist.next)
    {
        if (ir->cp == cp)
            return ir;
    }
    return (inbound_reservation_t *)0;
}

static bool target_in_accepted_list (
    unsigned target_num,
    const unsigned *accepted_list
)
//
// returns true if 'target_num' is one of the config.n_replicas in
// 'accepted_list'
//
{
    const unsigned *accepted_lim;
    
    assert (accepted_list);
    
    for (accepted_lim = accepted_list + config.n_replicas;
         accepted_list != accepted_lim;
         ++accepted_list)
    {
        if (*accepted_list == target_num)
            return true;
    }
    return false;
}

static void ir_remove (rep_target_t *tp,inbound_reservation_t *ir)
//
// remove inbound_reservation from the target linked list holding.
// decrement target's count of inbound_reservations
// free the removed node
//
{
#ifndef NDEBUG
    inbound_reservation_t *found_ir;
#endif
 
    assert(replicast);
    assert(ir);
    assert(tp);
    assert(tp->ir_queue_depth);
    
#ifndef NDEBUG
    found_ir = ir_find_by_cp(tp,ir->cp);
    assert(found_ir == ir);
#endif
    
    tllist_remove((tllist_t *)ir);
    free(ir);

    --tp->ir_queue_depth;
    assert(tp->ir_queue_depth >= 0);
    --total_reservations;
}

void handle_rep_chunk_put_accept_received (const event_t *e)
//
// When the Gateway has Accepted a Rendezvous Transfer it tells the entire
// Negotiating Group the set of accepted targets, and when the rendezvouos
// transfer will occur at.
//
// If this target is accepted the window must be adjusted to the actual
// scheduled transfer times (it was created to be for a longer duration)
// Otherwise the inbound_reservation can be erased.
//
{
    const rep_chunk_put_accept_t *cpa = (const rep_chunk_put_accept_t *)e;
    rep_target_t *tp;
    inbound_reservation_t *ir;

    assert(replicast);
    assert(cpa);
    assert(cpa->target_num < derived.n_targets);
    assert(chunk_seq(cpa->cp));
    tp = rept + cpa->target_num;
    ir = ir_find_by_cp (tp,cpa->cp);
    assert(ir);

    if (target_in_accepted_list(cpa->target_num,cpa->accepted_target)) {
        ir->tllist.time = cpa->window_start;
        ir->lim = cpa->window_lim;
        ir->accepted = true;
        ++tp->chunks_put;
    }
    else {
        ir_remove (tp,ir);
    }
}

void handle_rep_rendezvous_xfer_received (const event_t *e)
//
// When a specific target receives a complete valid Rendevous Transfer
// it can then enqueue the received chunk to be written to the target drive.
//
// In the simulation we assume that there were no transmission errors and we
// predict the length of the write. We would actually check the data in a real
// system and we would wait asynchrously for the write to actually complete.
//
// In the simulation we find the maching in_bound_reservation, and determine
// when the disk write would start (based on the current estimated completion
// for the target drive). It is the later of the prior scheduled completion or
// 'now' for this event. The time to write the payload is added, so that the
// disk_write_completion can be scheduled.
//
{
    const rep_rendezvous_xfer_received_t *rtr =
        (const rep_rendezvous_xfer_received_t *)e;
    rep_target_t *tp = rept + rtr->target_num;
    inbound_reservation_t *ir = ir_find_by_cp(tp,rtr->cp);
    tick_t write_start,write_complete;
    disk_write_start_t dws;
    tick_t write_variance =
	    derived.chunk_disk_write_duration/config.write_variance;;
    tick_t write_duration = derived.chunk_disk_write_duration
                            - write_variance/2
                            +  (rand() % write_variance);

    
    assert(replicast);
    assert(e);
    assert(rtr->target_num < derived.n_targets);
    assert(chunk_seq(rtr->cp));
    ir = ir_find_by_cp(tp,rtr->cp);
    assert(ir);
    ir_remove(tp,ir);
    
    write_start = (tp->last_disk_write_completion > e->tllist.time)
        ? tp->last_disk_write_completion
        : e->tllist.time;

    dws.event.create_time = e->tllist.time;
    dws.event.tllist.time = write_start;
    write_complete = write_start + write_duration;
    tp->last_disk_write_completion = write_complete;
    dws.expected_done = write_complete;
    dws.event.type = DISK_WRITE_START;
    dws.cp = rtr->cp;
    dws.target_num = rtr->target_num;
    if ((dws.write_qdepth = tp->common.write_qdepth++) == 0)
        ++track.n_active_targets;
    dws.qptr = &tp->common.write_qdepth;
    insert_event(dws);
}

#define MAX_TALLY 2048
void report_rep_chunk_distribution (void)

// Report distribution of chunks to targets to log_f

{
    unsigned tally [MAX_TALLY];
    const rep_target_t *tp;
    const rep_target_t *tp_lim;
    unsigned n,max_n;
    
    memset(tally,0,sizeof tally);
    for (tp =  rept, tp_lim = rept + derived.n_targets, max_n = 0;
         tp != tp_lim;
         ++tp)
    {
        n = tp->chunks_put;
        if (n >= MAX_TALLY) n = MAX_TALLY-1;
        ++tally[n];
        if (n > max_n) max_n = n;
    }
    fprintf(log_f,"Chunks per target distribution:\n");
    for (n = 0;;++n) {
        fprintf(log_f,"%d --> %d\n",n,tally[n]);
        if (n == max_n) break;
    }
}

