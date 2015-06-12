//
//  replicast_target.c
//  StorageClusterSim
//
//  Created by cait on 5/13/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#include "storage_cluster_sim.h"

static unsigned total_reservations = 0;

typedef struct rep_target_t {       // Track replicast target
    target_t    common;             // common fields
    inbound_reservation_t ir_head;  // tllist of inbound reservations
    unsigned ir_queue_depth;        // # of inbound reservations
    tick_t last_disk_write_completion;  // last disk write completion for
                                        // this target
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

static void make_bid (unsigned target_num,
                      chunk_put_handle_t cp,
                      tick_t *start,
                      tick_t *lim,
                      unsigned *qdepth)

// Make a bid to receive 'chunk_num' on 'target_num' later than 'now' lasting
// at least 'duration'. Record this bid as a new inbound_reservation for the
// target and in *start and *lim
//
// duration should already be padded to allow the Gateway to match varying bids
//
// TODO: also calculate a "jit_start" which is the earliest time when the
// rendezvous transfer would be able to proceed directly to disk. That is,
// this is the latest rendezvous transfer time that will still return the
// earlie chunk ack at least from this Target.
//
// the gateway will adjust the window based on the jit_start and/or break
// ties in favor of earlier jit_starts.

{
    inbound_reservation_t *ir =
        (inbound_reservation_t *)calloc(1,sizeof(inbound_reservation_t));
    inbound_reservation_t *p;
    inbound_reservation_t *insert_after;
    rep_target_t *tp = rept + target_num;
    tick_t s;

    assert(start);
    assert(ir);
    assert(target_num < derived.n_targets);
    assert(replicast);
    
    // make initial guess
    *start = s = now + 2*config.cluster_trip_time;
    *lim = s + derived.chunk_udp_xmit_duration*3;
    
    for (p = (inbound_reservation_t *)tp->ir_head.tllist.next;
         p != &tp->ir_head;
         p = (inbound_reservation_t *)p->tllist.next)
    {
        if (p->lim < *start) continue; // is is over before this bid starts
        if (p->tllist.time > *lim) break;  // remaianing reservations are later
        if (!p->accepted) {
            static unsigned n_tentative_conficts = 0;
            
            ++n_tentative_conficts;
        }
        else {
            static unsigned n_accepted_conflicts = 0;
            
            ++n_accepted_conflicts;
        }
        // adjust guess to be after inbound_reservation p
        *start = p->lim + 1;
        *lim = *start + derived.chunk_udp_xmit_duration*2;
    }
    ir->tllist.time = *start;
    ir->lim = *lim;
    ir->cp = cp;
    assert(chunk_seq(cp));
    assert(ir->tllist.time < ir->lim);
    
    insert_after =
        (inbound_reservation_t *)tllist_find ((tllist_t *)&tp->ir_head,*start);
    tllist_insert ((tllist_t *)insert_after,(tllist_t *)ir);
    assert(qdepth);
    *qdepth = tp->ir_queue_depth++;
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
    cpresp.event.tllist.time = e->tllist.time + config.cluster_trip_time;
    cpresp.event.type = REP_CHUNK_PUT_RESPONSE_RECEIVED;
    cpresp.cp = cpr->cp;
    cpresp.target_num = cpr->target_num;
    make_bid(cpresp.target_num,cpresp.cp,
             &cpresp.bid_start,&cpresp.bid_lim,&cpresp.qdepth);
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
    inbound_reservation_t *found_ir;
 
    assert(replicast);
    assert(ir);
    assert(tp);
    assert(tp->ir_queue_depth);
    
    found_ir = ir_find_by_cp(tp,ir->cp);
    assert(found_ir == ir);
    
    tllist_remove((tllist_t *)ir);
    free(ir);

    --tp->ir_queue_depth;
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
    tp = rept + cpa->target_num;
    if (target_in_accepted_list(cpa->target_num,cpa->accepted_target)) {
        ir->tllist.time = cpa->window_start;
        ir->lim = cpa->window_lim;
        ir->accepted = true;
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
    tick_t write_start;
    disk_write_completion_t dwc;
    
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
    
    dwc.event.create_time = e->tllist.time;
    dwc.event.tllist.time = write_start + derived.chunk_disk_write_duration;
    tp->last_disk_write_completion = dwc.event.tllist.time;
    dwc.event.type = DISK_WRITE_COMPLETION;
    dwc.cp = rtr->cp;
    dwc.target_num = rtr->target_num;
    dwc.write_qdepth = tp->common.write_qdepth++;
    dwc.qptr = &tp->common.write_qdepth;
    insert_event(dwc);
}

