//
//  replicast_gateway.c
//  StorageClusterSim
//
//  Created by cait on 8/10/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#include "storage_cluster_sim.h"

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
    const chunkput_t *c = (const chunkput_t *)cp;
    unsigned max_qdepth;
    unsigned bids_base;
    bid_t *b;
    
    assert (nbids <= config.n_targets_per_ng);
    
    qsort(bids,nbids,sizeof(bid_t),bid_compare);
    if (!config.terse) {
        for (n = 0; n != nbids; ++n) {
            fprintf(bid_f,"BIDS:CP #,%d,Start,%ld,Lim,%ld,Qdepth,%d,Tgt,%d",
                    c->seqnum,bids[n].start,bids[n].lim,bids[n].queue_depth,
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
            c->seqnum,now,start,lim);
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

void handle_rep_chunk_put_response_received (const event_t *e)
{
    const rep_chunk_put_response_received_t *cprr =
    (const rep_chunk_put_response_received_t *)e;
    rep_chunk_put_accept_t accept_event;
    rep_rendezvous_xfer_received_t rendezvous_xfer_event;
    chunkput_t *cp = (chunkput_t *)cprr->cp;
    chunkput_t *new_cp;
    tick_t next_chunk_time;
    
    assert(cp);
    assert(!cp->mbz);
    assert(cp->sig == 0xABCD);
    assert(cp->seqnum);
    assert(0 < cp->u.replicast.responses_uncollected);
    assert(cp->u.replicast.responses_uncollected <= config.n_targets_per_ng);
    
    save_bid (cp->u.replicast.bids,&cp->u.replicast.nbids,cprr);
    if (--cp->u.replicast.responses_uncollected) return;
    
    accept_event.event.create_time = e->tllist.time;
    accept_event.event.tllist.time = e->tllist.time + config.cluster_trip_time;
    accept_event.event.type = REP_CHUNK_PUT_ACCEPT_RECEIVED;
    accept_event.cp = cprr->cp;
    
    memset(&accept_event.accepted_target[0],0,
           sizeof accept_event.accepted_target);
    
    select_replicast_targets (cprr->cp,cp->u.replicast.nbids,
                              cp->u.replicast.bids,
                              accept_event.accepted_target,
                              &accept_event.window_start,
                              &accept_event.window_lim);
    
    accept_event.event.create_time = now+1;
    rendezvous_xfer_event.event.create_time = now+1;
    rendezvous_xfer_event.event.tllist.time = accept_event.window_lim;
    assert(accept_event.window_start < accept_event.window_lim);
    assert(accept_event.window_lim > now);
    
    rendezvous_xfer_event.event.type = REP_RENDEZVOUS_XFER_RECEIVED;
    rendezvous_xfer_event.cp = accept_event.cp;
    
    for_ng(accept_event.target_num,cp->u.replicast.ng)
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
    //
    // schedule the next put request to start slightly before this rendezvous
    // transfer will complete
    //
    next_chunk_time = rendezvous_xfer_event.event.tllist.time;
    next_chunk_time -= 3*config.cluster_trip_time;
    if (next_chunk_time <= now) next_chunk_time = now+1;
    if ((new_cp = next_cp(cp->gateway)) != NULL)
        insert_next_chunk_put_ready(new_cp,next_chunk_time);
}

void log_rep_chunk_put_response_received (FILE *f,const event_t *e)
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

#define MINIMUM_UDPV6_BYTES 66
#define CHUNK_PUT_REQUEST_BYTES (MINIMUM_UDPV6_BYTES+200)

void handle_rep_chunk_put_ready (const event_t *e)
{
    const chunk_put_ready_t *cpr = (const chunk_put_ready_t *)e;
    chunkput_t *cp = (chunkput_t *)cpr->cp;
    rep_chunk_put_request_received_t cprr;
    
    assert (cp);
    assert(!cp->mbz);
    
    cprr.event.create_time = e->tllist.time;
    cprr.event.tllist.time   = e->tllist.time +
    config.cluster_trip_time + CHUNK_PUT_REQUEST_BYTES*8;
    cprr.event.type = REP_CHUNK_PUT_REQUEST_RECEIVED;
    cprr.cp = (chunk_put_handle_t)cp;
    cp->u.replicast.responses_uncollected = config.n_targets_per_ng;
    
    /* for each Target in randomly selected negotiating group.
     * the Targets are assigned round-robin to Negotiating Groups.
     *
     * Actual Negotiating Groups are selected based on cryptographic hash
     * of the payload (for payload chunks) or the object name (for metadata)
     */
    cp->u.replicast.ng = rand() % config.n_negotiating_groups;
    for_ng(cprr.target_num,cp->u.replicast.ng)
        insert_event(cprr);
}

void log_rep_chunk_put_ready (FILE *f,const event_t *e)
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