//
//  replicast.c
//  StorageClusterSim
//
//  Created by Caitlin Bestler on 8/17/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//


#include "storage_cluster_sim.h"

static unsigned total_reservations = 0;

typedef struct chunkput_replicast {
    chunkput_t  cp;
    bid_t    bids[MAX_TARGETS_PER_NG];
    // collected bid response
    // once rendezvous transfer is scheduled it is stored in bids[0].
    unsigned nbids; // # of bids collected
    unsigned responses_uncollected; // Chunk Put responses still to be collected
} chunkput_replicast_t;

typedef enum replicast_event_type  { // exends enum event_type
    REP_CHUNK_PUT_READY = TRANSPORT_EVENT_BASE,
    REP_CHUNK_PUT_REQUEST_RECEIVED,
    REP_CHUNK_PUT_RESPONSE_RECEIVED,
    REP_CHUNK_PUT_ACCEPT_RECEIVED,
    REP_RENDEZVOUS_XFER_RECEIVED
} replicast_event_type_t;

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

typedef struct rep_chunk_put_accept {
    event_t event;          // chunk_put_accept is an event
    chunk_put_handle_t  cp; // Handle of the chunk put
    tick_t  window_start;   // The start of the accepted window
    tick_t  window_lim;     // The end of the accepted window
    unsigned target_num;    // Target_num of the target receiving this message
    unsigned accepted_target[MAX_REPLICAS]; // the accepted targets
} rep_chunk_put_accept_t;
//
// The Chunk Put Accept message is the 3rdc step in the Replicast Negotiation.
//
// it is sent by the originating Gateway to each target in the Negotiating Group
// to tell them what subset of the group has been selected for the Rendezvous
// Transfer, and what subset of offered reserved inbound window has been
// accepted.


typedef struct rep_rendezvous_xfer_received {
    event_t event;          // rep_rendezvous_transfer_receieved is an event
    chunk_put_handle_t  cp; // Handle of the chunk put
    unsigned target_num;    // The target that received this rendezvous transfer
} rep_rendezvous_xfer_received_t;
//
// The Rendezvous Transfer is received by each selected target for a specific
// chunk put. it occurs when the full chunk transfer would be compelte.
//
// It is scheduled at the same time as the chunk_put_accept for the subset
// of the negotiating group which was accepted.
//

static rep_target_t *repu = NULL;

#define for_ng(target,ng) \
    for ((target) = (ng);\
    (target) < derived.n_targets;\
    (target) += config.n_negotiating_groups)

//
// expands to a for loop which will walk 'target_num' thru the set of all
// targets defined for negotiating group 'ng'
//
// The for body must follow this macro
//

#define TRACK_PER_NG 1000
typedef struct ng {
    tick_t *next;
    tick_t ring[TRACK_PER_NG];
} ng_t;

static ng_t *ngs = NULL;

static void track_ng_unsolicited (unsigned ng)
{
    ng_t *n = ngs + ng;

    assert(ng < config.n_negotiating_groups);
    assert(ngs);
    
    *n->next = now;
    if (++n->next == n->ring + TRACK_PER_NG)
        n->next = n->ring;
}

// TODO: this should eventually be moved to a location common for all
// protocols using negotiating groups

#define RATE_THRESHOLD 1000000
static unsigned unsolicited_rate (unsigned ng)
{
    ng_t *n = ngs + ng;
    tick_t *p;
    unsigned n_counted;
    
    assert(ng < config.n_negotiating_groups);
    assert(ngs);
    for (p = n->next,n_counted = 0;;++n_counted) {
        --p;
        if (p == n->ring - 1)
            p = n->ring + (TRACK_PER_NG-1);

        if (*p == ~0L) break;
        if (now - *p > RATE_THRESHOLD) break;
        if (p == n->next) break;
    }
    return n_counted;
}

static void save_bid (bid_t *bids,
                      unsigned *nbids,
                      const rep_chunk_put_response_received_t *cprr)
//
// save the bid within 'cpr' in bids[n*bids], then increment *nbids
// verify that *nbids <= N_TARGETS_PER_NG
//
{
    bid_t *b;
    assert(cprr);
    assert(cprr->bid_start < cprr->bid_lim);
    assert(cprr->target_num < derived.n_targets);
    assert (nbids);
    assert (*nbids <= config.n_targets_per_ng);
    assert(cprr->qdepth >= 0);
    assert(cprr->qdepth < 999);
    
    b = bids + *nbids;
    b->start = cprr->bid_start;
    b->lim = cprr->bid_lim;
    b->estimated_ack = cprr->estimated_ack;
    b->target_num = cprr->target_num;
    b->queue_depth = cprr->qdepth;
    ++*nbids;
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

static void select_replicast_targets (chunk_put_handle_t cp,
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
    const chunkput_replicast_t *c = (const chunkput_replicast_t *)cp;
    unsigned max_qdepth;
    unsigned bids_base;
    bid_t *b;
    
    assert (nbids <= config.n_targets_per_ng);
    
    qsort(bids,nbids,sizeof(bid_t),bid_compare);
    if (!config.terse) {
        for (n = 0; n != nbids; ++n) {
            fprintf(bid_f,"BIDS:CP #,%d,Start,%ld,Lim,%ld,Qdepth,%d,Tgt,%d",
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
    
    // if (0 && m > config.n_replicas) // make it confurable: disregard target qdepths
    if (m > config.n_replicas)
        qsort(b,m,sizeof(bid_t),estimate_compare);
    *window_start = start = b->start;
    assert (start > now);
    *window_lim = lim = start + derived.chunk_udp_xmit_duration;
    assert (lim > start);
    
    fprintf(bid_f,"BIDS:CP #,%d,Now,%ld,Accepted,%ld,%ld,TARGET",
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

static bool target_in_accepted_list (const unsigned *accepted,unsigned target)
//
// returns true if 'target' is one of the N_REPLICA targets at 'accepted'
//
{
    const unsigned *p;
    
    for (p = accepted; p != accepted+config.n_replicas; ++p)
        if (*p == target)
            return true;
    return false;
}

static void handle_rep_chunk_put_response_received (const event_t *e)
{
    const rep_chunk_put_response_received_t *cprr =
    (const rep_chunk_put_response_received_t *)e;
    rep_chunk_put_accept_t accept_event;
    rep_rendezvous_xfer_received_t rendezvous_xfer_event;
    chunkput_replicast_t *cp = (chunkput_replicast_t *)cprr->cp;
    chunkput_t *new_cp;
    tick_t next_chunk_time;
    
    assert(cp);
    assert(!cp->cp.mbz);
    assert(cp->cp.sig == 0xABCD);
    assert(cp->cp.seqnum);
    assert(0 < cp->responses_uncollected);
    assert(cp->responses_uncollected <= config.n_targets_per_ng);
    
    save_bid (cp->bids,&cp->nbids,cprr);
    if (--cp->responses_uncollected) return;
    
    accept_event.event.create_time = now;
    accept_event.event.tllist.time = now + config.cluster_trip_time;
    accept_event.event.type = (event_type_t)REP_CHUNK_PUT_ACCEPT_RECEIVED;
    accept_event.cp = cprr->cp;
    
    memset(&accept_event.accepted_target[0],0,
           sizeof accept_event.accepted_target);
    
    select_replicast_targets (cprr->cp,cp->nbids,cp->bids,
                              accept_event.accepted_target,
                              &accept_event.window_start,
                              &accept_event.window_lim);
    
    rendezvous_xfer_event.event.create_time = now+1;
    rendezvous_xfer_event.event.tllist.time = accept_event.window_lim;
    assert(accept_event.window_start < accept_event.window_lim);
    assert(accept_event.window_lim > now);
    
    rendezvous_xfer_event.event.type =
        (event_type_t)REP_RENDEZVOUS_XFER_RECEIVED;
    rendezvous_xfer_event.cp = accept_event.cp;
    
    for_ng(accept_event.target_num,cp->cp.ng)
    {
        /* Insert the ACCEPT message for all members of the Negotiating Group
         * Insert the Rendezvous Transfer for the accepted members
         */
        insert_event(accept_event);
        if (target_in_accepted_list(accept_event.accepted_target,
                                    accept_event.target_num))
        {
            rendezvous_xfer_event.target_num = accept_event.target_num;
            insert_event(rendezvous_xfer_event);
        }
    }
    track_ng_unsolicited(cp->cp.ng);
    //
    // schedule the next put request to start slightly before this rendezvous
    // transfer will complete
    //
    next_chunk_time = rendezvous_xfer_event.event.tllist.time;
    next_chunk_time -= 3*config.cluster_trip_time;
    if (next_chunk_time <= now) next_chunk_time = now+1;
    if ((new_cp = next_cp(cp->cp.gateway,replicast_prot.cp_size)) != NULL)
        insert_next_chunk_put_ready(new_cp,next_chunk_time);
}

static void log_rep_chunk_put_response_received (FILE *f,const event_t *e)
{
    const rep_chunk_put_response_received_t *cpresp =
    (rep_chunk_put_response_received_t *)e;
    
    if (!config.terse) {
        assert(e);
        fprintf(f,"0x%lx,0x%lx,REP_CHUNK_PUT_RESPONSE_RCVD,0x%lx,%d,",
                e->tllist.time,e->create_time,cpresp->cp,chunk_seq(cpresp->cp));
        fprintf(f,"tgt,%d,bid,0x%lx,0x%lx\n",cpresp->target_num,
                cpresp->bid_start,cpresp->bid_lim);
    }
}

static target_t *rep_target (unsigned target_num)
{
    return &repu[target_num].common;
}

static unsigned n_active_for_ng (unsigned ng)
{
    unsigned tgt;
    unsigned n_active = 0;
    const rep_target_t *t;
    
    for_ng(tgt,ng) {
        t = (const rep_target_t *)rep_target(tgt);
        if (t->ir_queue_depth > 0)
            ++n_active;
    }
    return n_active;
}

#define UNSOLICITED_BUDGET_BITS (TICKS_PER_SECOND/10/10000)
// 1 Gbs budget for 1/10,000 th of a second

#define UNSOLICITED_BUDGET_PKTS (UNSOLICITED_BUDGET_BITS/8000)
// assume 1000 byte unsolicited packets

static tick_t congestion_delay (unsigned ng)
{
    unsigned n_unsolicited = unsolicited_rate(ng);
    unsigned n_active = n_active_for_ng(ng);
    signed congestion = n_unsolicited - (signed)UNSOLICITED_BUDGET_PKTS;
    tick_t delay;
    
    fprintf(log_f,"n_unsolicited,%d,#active,%d,ng,%d\n",n_unsolicited,
            n_active,ng);
    delay = (congestion <= 0)
        ? 0L
        : config.congestion_penalty*n_active*congestion;
    if (delay)
        fprintf(log_f,"Delay,%lu,ng,%d\n",delay,ng);
    return delay;
}

#define MINIMUM_UDPV6_BYTES 66
#define CHUNK_PUT_REQUEST_BYTES (MINIMUM_UDPV6_BYTES+200)

static void handle_rep_chunk_put_ready (const event_t *e)
{
    const chunk_put_ready_t *cpr = (const chunk_put_ready_t *)e;
    chunkput_replicast_t *cp = (chunkput_replicast_t *)cpr->cp;
    rep_chunk_put_request_received_t cprr;
    
    assert (cp);
    assert(!cp->cp.mbz);
    
    cprr.event.create_time = now;
    cprr.event.tllist.time   = now +
        config.cluster_trip_time + CHUNK_PUT_REQUEST_BYTES*8;
    cprr.event.type = (event_type_t)REP_CHUNK_PUT_REQUEST_RECEIVED;
    cprr.cp = (chunk_put_handle_t)cp;
    cp->responses_uncollected = config.n_targets_per_ng;
    
    cprr.event.tllist.time += congestion_delay (cp->cp.ng);
    
    // if negotiating group is congested then delay this chunk_put_request
    
    
    /* for each Target in randomly selected negotiating group.
     * the Targets are assigned round-robin to Negotiating Groups.
     *
     * Actual Negotiating Groups are selected based on cryptographic hash
     * of the payload (for payload chunks) or the object name (for metadata)
     */
    cp->cp.ng = rand() % config.n_negotiating_groups;
    for_ng(cprr.target_num,cp->cp.ng)
        insert_event(cprr);
    track_ng_unsolicited(cp->cp.ng);
}

static void log_rep_chunk_put_ready (FILE *f,const event_t *e)
{
    gateway_t *gateway;
    const chunk_put_ready_t *cpr = (const chunk_put_ready_t *)e;
    
    if (!config.terse) {
        assert(e);
        fprintf(f,"0x%lx,0x%lx,rep CHUNK_PUT_READY,0x%lx,%d",
                e->tllist.time,e->create_time,cpr->cp,chunk_seq(cpr->cp));
        gateway = chunk_gateway(cpr->cp);
        fprintf(f,",Gateway,%d,chunks,%d\n",gateway->num,gateway->n_chunks);
    }
}

static void init_rep_targets(unsigned n_targets)
//
// Initialize the target subsystem, specifically the irhead for each
// entry in the target_t array 't' must be set to empty.
// Setting the ir_head to point to itself makes an empty list.
//
{
    unsigned n;
    ng_t *ng;
    
    repu = (rep_target_t *)calloc(n_targets,sizeof(rep_target_t));
    assert(repu);
    
    for (n=0;n != n_targets;++n)
        repu[n].ir_head.tllist.next = repu[n].ir_head.tllist.prev =
            &repu[n].ir_head.tllist;
    ngs = (ng_t *)calloc(config.n_negotiating_groups,(sizeof *ngs));
    for (ng = ngs; ng != ngs + config.n_negotiating_groups;++ng) {
        ng->next = ng->ring;
        memset(ng->ring,0xFF,(sizeof ng->ring));
    }
}

static void release_rep_targets (void)
{
    free(repu);
    repu = (rep_target_t *)0;
    free(ngs);
    ngs = (ng_t *)0;
}

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
    rep_target_t *tp = repu + target_num;
    tick_t s;
    tick_t estimated_write_start;
    
    bool delayed;
    
    assert(start);
    assert(lim);
    assert(ack_at);
    assert(ir);
    assert(target_num < derived.n_targets);
    
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

static void handle_rep_chunk_put_request_received (const event_t *e)

// Generate a Chunk Put Response with a bid for the chunk put
// This involves making a bid which is for this target. It must:
//      Tentatively reserve 3x the transmission time required.
//      Not conflict with any existing inbound_reservation for this target

{
    const rep_chunk_put_request_received_t *cpr =
    (const rep_chunk_put_request_received_t *)e;
    rep_chunk_put_response_received_t cpresp;
    
    assert(e);
    
    cpresp.event.create_time = e->tllist.time;
    cpresp.event.tllist.time = e->tllist.time + config.cluster_trip_time +
        config.replicast_packet_processing_penalty;
    cpresp.event.type = (event_type_t)REP_CHUNK_PUT_RESPONSE_RECEIVED;
    cpresp.cp = cpr->cp;
    cpresp.target_num = cpr->target_num;
    make_bid(cpresp.target_num,cpresp.cp,&cpresp.bid_start,&cpresp.bid_lim,
             &cpresp.estimated_ack,&cpresp.qdepth);
    insert_event(cpresp);
}

static void log_rep_chunk_put_request_received (FILE *f,const event_t *e)
{
    const rep_chunk_put_request_received_t *cpreq =
    (const rep_chunk_put_request_received_t *)e;
    
    if (!config.terse) {
        assert(e);
        fprintf(f,"0x%lx,0x%lx,REP_CHUNK_PUT_REQUEST_RECEIVED,0x%lx,%d",
                e->tllist.time,e->create_time,cpreq->cp,chunk_seq(cpreq->cp));
        fprintf(f,",tgt,%d\n",cpreq->target_num);
    }
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

static void handle_rep_chunk_put_accept_received (const event_t *e)
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
    
    assert(cpa);
    assert(cpa->target_num < derived.n_targets);
    assert(chunk_seq(cpa->cp));
    tp = repu + cpa->target_num;
    ir = ir_find_by_cp (tp,cpa->cp);
    assert(ir);
    
    if (target_in_accepted_list(cpa->accepted_target,cpa->target_num)) {
        ir->tllist.time = cpa->window_start;
        ir->lim = cpa->window_lim;
        ir->accepted = true;
        ++tp->chunks_put;
    }
    else {
        ir_remove (tp,ir);
    }
}

static void log_rep_chunk_put_accept_received (FILE *f,const event_t *e)
{
    unsigned i;
    const rep_chunk_put_accept_t *cpa = (const rep_chunk_put_accept_t *)e;
    
    if (!config.terse) {
        fprintf(f, "0x%lx,0x%lx,REP_CHUNK_PUT_ACCEPT_RECEIVED,0x%lx,%d,CP,%d,",
                e->tllist.time,e->create_time,cpa->cp,chunk_seq(cpa->cp),
                cpa->target_num);
        fprintf(f,"%ld,%ld,targets,",cpa->window_start,cpa->window_lim);
        for (i=0;i != config.n_replicas;++i)
            fprintf (f,",%d",cpa->accepted_target[i]);
        fprintf(f,"\n");
    }
}

static void handle_rep_rendezvous_xfer_received (const event_t *e)
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
    rep_target_t *tp = repu + rtr->target_num;
    inbound_reservation_t *ir = ir_find_by_cp(tp,rtr->cp);
    tick_t write_start,write_complete;
    disk_write_start_t dws;
    tick_t write_variance =
    derived.chunk_disk_write_duration/config.write_variance;;
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
}

static void log_rep_rendezvous_xfer_received (FILE *f,const event_t *e)
{
    const rep_rendezvous_xfer_received_t *rtr =
    (const rep_rendezvous_xfer_received_t *)e;
    
    if (!config.terse) {
        fprintf(f,"0x%lx,0x%lx",e->tllist.time,e->create_time);
        fprintf(f,",REP_CHUNK_RENDEZVOUS_XFER_RCVD,CP,0x%lx,%d,tgt,%d\n",
                rtr->cp,chunk_seq(rtr->cp),rtr->target_num);
    }
}

#define MAX_TALLY 2048
static void report_rep_chunk_distribution (FILE *f)

// Report distribution of chunks to targets to log_f

{
    unsigned tally [MAX_TALLY];
    const rep_target_t *tp;
    const rep_target_t *tp_lim;
    unsigned n,max_n;
    
    memset(tally,0,sizeof tally);
    for (tp =  repu, tp_lim = repu + derived.n_targets, max_n = 0;
         tp != tp_lim;
         ++tp)
    {
        n = tp->chunks_put;
        if (n >= MAX_TALLY) n = MAX_TALLY-1;
        ++tally[n];
        if (n > max_n) max_n = n;
    }
    fprintf(f,"Chunks per target distribution:\n");
    for (n = 0;;++n) {
        fprintf(f,"%d --> %d\n",n,tally[n]);
        if (n == max_n) break;
    }
}

protocol_t replicast_prot = {
    .tag = "rep",
    .name = "Replicast-Multicast",
    .cp_size = sizeof(chunkput_replicast_t),
    .do_me = false,
    .init_target = init_rep_targets,
    .target = rep_target,
    .report_chunk_distribution = report_rep_chunk_distribution,
    .release_targets = release_rep_targets,
    .h = {
        {handle_rep_chunk_put_ready,log_rep_chunk_put_ready},
        {handle_rep_chunk_put_request_received,
            log_rep_chunk_put_request_received},
        {handle_rep_chunk_put_response_received,
            log_rep_chunk_put_response_received},
        {handle_rep_chunk_put_accept_received,
            log_rep_chunk_put_accept_received},
        {handle_rep_rendezvous_xfer_received,
            log_rep_rendezvous_xfer_received}
    }
};



