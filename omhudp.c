//
//  omhudp.c
//  StorageClusterSim
//
//  Created by cait on 8/21/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//
// Variation on replicast protocol where the gateway omnisiciently
// picks the perfect rendezvous transfer and simply executes it.
//
// This is of course only possible in simulation. A real-world
// implementation does not have the required data collected at
// any single point to reach such a determination.
//

#include "storage_cluster_sim.h"

typedef struct chunkput_omhudp {
    chunkput_t  cp;
    unsigned acked;
} chunkput_omhudp_t;

typedef enum omhudp_event_type { // extends enum event_type
    OMHUDP_CHUNK_PUT_READY = TRANSPORT_EVENT_BASE,
    OMHUDP_RENDEZVOUS_XFER_RECEIVED
} omhudp_event_type_t;

typedef struct omhudp_target_t {
    // Track a omniscient hash target
    target_t    common;             // common tracking fields
    inbound_reservation_t ir_head;  // tllist of inbound reservations
    int ir_queue_depth;        // # of inbound reservations
    tick_t last_disk_write_completion;  // last disk write completion for
                                        // this target
    unsigned chunks_put;
    unsigned mbz;   // debugging paranoia
} omhudp_target_t;

typedef struct omhudp_rendezvous_xfer_received {
    event_t event;          // rep_rendezvous_transfer_receieved is an event
    chunk_put_handle_t  cp; // Handle of the chunk put
    unsigned target_num;    // The target that received this rendezvous transfer
    bool is_first;
} omhudp_rendezvous_xfer_received_t;
//
// The Rendezvous Transfer is received by each selected target for a specific
// chunk put. it occurs when the full chunk transfer would be compelte.
//
// It is scheduled at the same time as the chunk_put_accept for the subset
// of the negotiating group which was accepted.
//

static omhudp_target_t *omhudp_tgt = NULL;

#define for_ng(target,ng) \
    for ((target) = (ng);\
    (target) < derived.n_targets;\
    (target) += config.n_negotiating_groups)

static inbound_reservation_t *ir_find_by_cp (
                                             omhudp_target_t *tp,
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

static void make_bid (unsigned target_num,bid_t *bid)

// Make a bid to receive 'chunk_num' on 'target_num' later than 'now' lasting
// at least 'duration'. Record this bid in '*bid'
//

{
    inbound_reservation_t *p;
    omhudp_target_t *tp = omhudp_tgt + target_num;
    tick_t s;
    bool delayed;

    
    assert(bid);
    assert(target_num < derived.n_targets);
    
    // make initial guess
    bid->start = s = now + 2*config.cluster_trip_time +
                 config.replicast_packet_processing_penalty;;
    bid->lim = s + derived.chunk_udp_xmit_duration;
    assert(bid->start < bid->lim);
    
    for (p = (inbound_reservation_t *)tp->ir_head.tllist.next, delayed = false;
         p != &tp->ir_head;
         p = (inbound_reservation_t *)p->tllist.next)
    {
        
        if (p->lim < bid->start) continue; // is is over before this bid starts
        if (p->tllist.time > bid->lim) break;  // remaianing reservations are later
        
        // adjust guess to be after inbound_reservation p
        bid->start = p->lim + 1;
        bid->lim = bid->start + derived.chunk_udp_xmit_duration;
        assert(bid->start < bid->lim);
        delayed = true;
    }
    ++track.n_reservations;
    if (delayed)
        ++track.n_reservation_conflicts;
}

static int bid_compare (const void *a,const void *b)
//
// Compare two bids for qsort() by start
//
{
    const bid_t *ba = a;
    const bid_t *bb = b;
    
    return ba->start < bb->start ? -1
    : ba->start == bb->start ? 0
    : 1;
}

static int estimate_compare (const void *a,const void *b)
//
// Compare two bids for qsort() by estimated ack
//
{
    const bid_t *ba = a;
    const bid_t *bb = b;
    
    return ba->estimated_ack < bb->estimated_ack ? -1
    : ba->estimated_ack == bb->estimated_ack ? 0
    : 1;
}

static unsigned acceptable_bid_set (bid_t *bids,unsigned nbids,unsigned *best)

// are there at least config.n_replicas bids that overlap with the
// bid in bid[0] for at least derived.chunk_udp_xmit_duration?
//
// If so
//      return the number of bids that comply.
//      This will be at least config.n_replicas.
//      If there are enough, it will be limited to those that start no
//      later than window_start + derived.chunk_udp_xmit_duration.
//      Otherwise config.n_replicas will be returned.
//      The ->start field will have been adjusted for bid[0] .. bid [n-1]
// Otherwise
//      return will be zero
//      *start will not be modified

{
    bid_t *b;
    bid_t *b_lim = bids + nbids;
    tick_t window_start;
    tick_t window_lim;
    tick_t span,best_span;
    unsigned n,m;
#ifndef NDEBUG
    unsigned b0target = bids[0].target_num;
#endif
    unsigned delta = config.n_replicas - 1;
    
    assert(best);
    
    // find the starting bid with the minimum delta to the +n_replicas bid
    for (m = n = 0,best_span = ~0L;n + delta < nbids; ++n) {
        assert(bids[n].start > now);
        span = bids[n+delta].start - bids[n].start;
        if (span < best_span) {
            m = n;
            best_span = span;
        }
    }
    *best = m;
    window_start = bids[m].start;
    window_lim = bids[m].lim;
    // find how many are a good match
    for (n = 0, b = bids + m, b_lim = bids + nbids; b != b_lim; ++n,++b) {
        assert(b->start < b->lim);
        assert(b->start > now);
        
        if (b->start > window_lim) break;
        if (n >= config.n_replicas &&
            b->start > window_start + derived.chunk_udp_xmit_duration)
            break;
        
        if (b->start > window_start) window_start = b->start;
        if (b->lim < window_lim) window_lim = b->lim;
    }
    if (n < config.n_replicas)
        return 0;
    for (m = 0; m <= n; ++m)
        bids[m].start = window_start;
    
    assert (b0target == bids[0].target_num);
    return n;
}

static void select_targets (const chunkput_omhudp_t *c,
                            unsigned nbids,
                            bid_t *bids,
                            unsigned *accepted_target,
                            tick_t *window_start,
                            tick_t *window_lim)

// sort collected bids, then find the earliest sequence of n_replica bids
// which have the required duration.
//
// Set the accepted targets (as target_nums) in 'accepted_target'
// Set the accepted window is bids[0]

{
    unsigned n,m;
    tick_t  start,lim;
    unsigned max_qdepth;
    unsigned bids_base;
    bid_t *b;
    
    assert (nbids <= config.n_targets_per_ng);
    
    qsort(bids,nbids,sizeof(bid_t),bid_compare);
    if (!config.terse) {
        for (n = 0; n != nbids; ++n) {
            fprintf(bid_f,"omhudp_BIDS:CP #,%d,Start,%ld,Lim,%ld,Qdepth,%d,Tgt,%d",
                    c->cp.seqnum,bids[n].start,bids[n].lim,bids[n].queue_depth,
                    bids[n].target_num);
            fprintf(bid_f,",EstAck,%ld\n",bids[n].estimated_ack);
        }
    }
    m = acceptable_bid_set(bids,nbids,&bids_base);
    assert(m);
    // Because every member of the negotiating group MUST bid we should
    // always find an acceptable set. A real storage cluster would re-bid
    // starting here to deal with temporary disruptions caused by temporary
    // loss of network connectivity.
    
    b = bids + bids_base;
    
    if (m > config.n_replicas)
        qsort(b,m,sizeof(bid_t),estimate_compare);
    *window_start = start = b->start;
    assert (start > now);
    *window_lim = lim = start + derived.chunk_udp_xmit_duration;
    assert (lim > start);
    
    fprintf(bid_f,"omhudp_BIDS:CP #,%d,Now,%ld,Accepted,%ld,%ld,TARGET",
            c->cp.seqnum,now,start,lim);
    for (m = 0,max_qdepth = 0; m != config.n_replicas; ++m) {
        if (b[m].queue_depth > max_qdepth)
            max_qdepth = b[m].queue_depth;
        accepted_target[m] = b[m].target_num;
        fprintf(bid_f,",%d",accepted_target[m]);
        inc_target_total_queue(b[m].target_num);
    }
    fprintf(bid_f,",index,%d,MaxQ,%d\n",m,max_qdepth);
    return;
}

static void omniscient_target_select(chunkput_omhudp_t *cp,
                                     unsigned *accepted_target,
                                     tick_t *start,
                                     tick_t *lim)
//
// just using targete data:
//      for each member of the ng
//          make bid
//      select targets
//      for each accepted target
//          make inbound reservation
//
{
    unsigned target;
    unsigned n = 0;
    bid_t bid[MAX_TARGETS_PER_NG];
    inbound_reservation_t *ir;
    omhudp_target_t *tp;
    inbound_reservation_t *insert_after;
    
    assert (cp);
    assert (accepted_target);
    assert (start);
    assert (lim);
    
    for_ng(target,cp->cp.ng) {
        make_bid(target,bid+n);
        bid[n].target_num = target;
        ++n;
    }
    select_targets(cp,config.n_targets_per_ng,bid,accepted_target,start,lim);
    for (n = 0; n != config.n_replicas; ++n) {
        ir = (inbound_reservation_t *)calloc(1,sizeof(inbound_reservation_t));
        ir->accepted = true;
        ir->cp = (chunk_put_handle_t)cp;
        ir->tllist.time = *start;
        ir->lim = *lim;
        tp = omhudp_tgt + accepted_target[n];
        insert_after = (inbound_reservation_t *)
            tllist_find((tllist_t *)&tp->ir_head,*start);
        tllist_insert ((tllist_t *)insert_after,(tllist_t *)ir);
        ++tp->ir_queue_depth;
        assert(tp->ir_queue_depth < 999);
    }
}

#define MINIMUM_TCPV6_BYTES 74
#define TCP_CHUNK_SETUP_BYTES (3*MINIMUM_TCPV6_BYTES+200)
// 3 packets for TCP connectino setup plus minimal pre-transfer data
// The cluster_trip_time must still be added to this.

static void handle_omhudp_chunk_put_ready (const event_t *e)
{
    const chunk_put_ready_t *cpr = (const chunk_put_ready_t *)e;
    chunkput_omhudp_t *cp = (chunkput_omhudp_t *)cpr->cp;
    omhudp_rendezvous_xfer_received_t xfer_event;
    unsigned accepted_target[MAX_REPLICAS];
    tick_t start,lim;
    unsigned n;
    
    assert (cp);
    assert(!cp->cp.mbz);
    
    omniscient_target_select(cp,accepted_target,&start,&lim);
    xfer_event.event.create_time = now+1;
    xfer_event.event.tllist.time = lim;
    xfer_event.event.type = (event_type_t)OMHUDP_RENDEZVOUS_XFER_RECEIVED;
    xfer_event.cp = (chunk_put_handle_t)cp;
    xfer_event.is_first = true;
    for (n = 0; n != config.n_replicas; ++n) {
        xfer_event.target_num = accepted_target[n];
        insert_event(xfer_event);
        xfer_event.is_first = false;
    }
}

static void log_omhudp_chunk_put_ready (FILE *f,const event_t *e)
{
    fprintf(f,"omhtcp,CHUNK_PUT_READY\n");
}

target_t *omhudp_target (unsigned target_num)
{
    return &omhudp_tgt[target_num].common;
}

void init_omhudp_targets(unsigned n_targets)
//
// Initialize the target subsystem, specifically the irhead for each
// entry in the target_t array 't' must be set to empty.
// Setting the ir_head to point to itself makes an empty list.
//
{
    unsigned n;
    
    assert(!omhudp_tgt);
    
    omhudp_tgt = (omhudp_target_t *)calloc(n_targets,sizeof(omhudp_target_t));
    
    assert(omhudp_tgt);
    for (n = 0;n != n_targets;++n) {
        omhudp_tgt[n].ir_head.tllist.next =
        omhudp_tgt[n].ir_head.tllist.prev =
        &omhudp_tgt[n].ir_head.tllist;
    }
}

void release_omhudp_targets (void)
{
    assert(omhudp_tgt);
    
    free(omhudp_tgt);
    omhudp_tgt = (omhudp_target_t *)0;
}

static void ir_remove (omhudp_target_t *tp,inbound_reservation_t *ir)
//
// remove inbound_reservation from the target linked list holding.
// decrement target's count of inbound_reservations
// free the removed node
//
{
#ifndef NDEBUG
    inbound_reservation_t *found_ir;
#endif
    
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
}

void handle_omhudp_rendezvous_xfer_received (const event_t *e)

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
    const omhudp_rendezvous_xfer_received_t *rtr =
        (const omhudp_rendezvous_xfer_received_t *)e;
    omhudp_target_t *tp = omhudp_tgt + rtr->target_num;
    inbound_reservation_t *ir = ir_find_by_cp(tp,rtr->cp);
    chunkput_omhudp_t *cp = (chunkput_omhudp_t *)rtr->cp;
    chunkput_t *new_cp;
    tick_t write_start,write_complete;
    disk_write_start_t dws;
    tick_t write_variance =
        derived.chunk_disk_write_duration/config.write_variance;
    tick_t write_duration = derived.chunk_disk_write_duration
                            - write_variance/2
                            +  (rand() % write_variance);
    
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
    
    insert_event(dws);
    //
    // schedule the next put request to start
    //
    if (rtr->is_first) {
        if ((new_cp = next_cp(cp->cp.gateway,omhudp_sim.cp_size)) != NULL) {
            insert_next_chunk_put_ready(new_cp,now+1);
        }
    }
}

void log_omhudp_rendezvous_xfer_received (FILE *f,const event_t *e)
{
    const tcp_xmit_received_t *txr = (const tcp_xmit_received_t *)e;
    
    if (!config.terse) {
        assert(e);
        fprintf(f,"0x%lx,0x%lx,omhudp_rendezvous_xfer,0x%lx,%d,tgt,%d\n",
                e->tllist.time,e->create_time,txr->cp,chunk_seq(txr->cp),
                txr->target_num);
    }
}



#define MAX_TALLY 2048
void report_omhudp_chunk_distribution (FILE *f)

// Report distribution of chunks to targets to log_f

{
    unsigned tally[MAX_TALLY];
    const omhudp_target_t *tp;
    const omhudp_target_t *tp_lim;
    unsigned n,max_n;
    
    memset(tally,0,sizeof tally);
    for (tp =  omhudp_tgt, tp_lim = omhudp_tgt + derived.n_targets, max_n = 0;
         tp != tp_lim;
         ++tp)
    {
        n = tp->chunks_put;
        if (n >= MAX_TALLY) n = MAX_TALLY-1;
        ++tally[n];
        if (n > max_n) max_n = n;
    }
    fprintf(f,"OMHTCP Chunks per target distribution:\n");
    for (n = 0;;++n) {
        fprintf(f,"%d --> %d\n",n,tally[n]);
        if (n == max_n) break;
    }
}

protocol_t omhudp_sim = {
    .tag = "omhudp",
    .name = "Omniscient Hash-UDP",
    .cp_size = sizeof(chunkput_omhudp_t),
    .init_target = init_omhudp_targets,
    .target = omhudp_target,
    .report_chunk_distribution = report_omhudp_chunk_distribution,
    .release_targets = release_omhudp_targets,
    .h = {
        {handle_omhudp_chunk_put_ready,log_omhudp_chunk_put_ready},
        {handle_omhudp_rendezvous_xfer_received,
            log_omhudp_rendezvous_xfer_received}
    }
};







