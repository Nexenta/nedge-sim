
//
//  gateway.c
//  StorageClusterSim
//
//  Created by Caitlin Bestler on 7/9/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#include "storage_cluster_sim.h"
#include <math.h>





//
// Extraordinarily Generous TCP modeling
//
// This simulation credits TCP with more brains than it actually has.
//
// We presume a congestion control algorithm that *insantly* adjusts to the
// current number of flows, and can run at 100% capacity. It could never be
// that good. But by modelling an obtainable limit we allow for future
// congestion control algorithm improvements. The simuation will still show
// the benefits of multicast communications no matter how generously we model
// TCP congestion control.
//
// Some other simplifications (also too generous)
//
//      We do not model the reverse flow acks at all. We assume they arrive in
//      time and never slow delivery down. TCP does an excellent job of
//      minimizing the traffic impact of its acks, but does not reach this
//      level of non-interference.
//
//      Real TCP connection setup is not congestion controlled. Replicast
//      Rendezvous negotiations are. We generously assume no retransmissions
//      for unsolicited packets, but Replicast will actually take stakes to
//      achieve this.

#define MINIMUM_TCPV6_BYTES 74
#define TCP_CHUNK_SETUP_BYTES (3*MINIMUM_TCPV6_BYTES+200)
    // 3 packets for TCP connectino setup plus minimal pre-transfer data
    // The cluster_trip_time must still be added to this.


static unsigned n_chunkputs = 0;

chunkput_t *next_cp (void)
{
    chunkput_t *cp;
    
    if (now > config.sim_duration) return (chunkput_t *)0;

    cp = (chunkput_t *)calloc(1,sizeof(chunkput_t));
    assert(cp);
    assert(!cp->mbz);
    
    cp->sig = 0xABCD;
    cp->seqnum = ++n_chunkputs;
    ++track.n_initiated;
    cp->started = now;
    cp->replicas_unacked = config.n_replicas;
    assert(cp->replicas_unacked);
    
    if (replicast)
        cp->u.replicast.ng = rand() % config.n_negotiating_groups;
    
    return cp;
}

static void next_tcp_replica_xmit (chunkput_t *cp,tick_t time_now)

// Schedule the next TCP transmit start after the previous tcp transmit for
// the same object has completed

{
    tcp_xmit_received_t txr;
    unsigned r;
    
    if (cp->replicas_unacked) {
        txr.event.create_time = time_now;
        txr.event.tllist.time = time_now + config.cluster_trip_time*3 +
                                TCP_CHUNK_SETUP_BYTES*8;
        txr.event.type = TCP_XMIT_RECEIVED;
        txr.cp = (chunk_put_handle_t)cp;
        r = cp->u.nonrep.repnum++;
        txr.target_num = cp->u.nonrep.ch_targets[r];
        insert_event(txr);
    }
}

void init_seqnum(void)

// re-initialize the chunk sequence numbers for another run through the chunks.

{
    n_chunkputs = 0;
}

static void select_nonrep_targets (chunkput_t *c)

// select config.n_replicas different targets
// store them in c->u.nonrep.ch_targets[0..n_repicas-1]
//
// for each pick select at random
//      if previously selected try next target

{
    unsigned n,t,i;
    
    assert (config.n_replicas < derived.n_targets);
    
    fprintf(log_f,"@0x%lx,CP,0x%p,%d,targets selected",now,c,c->seqnum);
    for (n = 0; n != config.n_replicas;++n) {
        t = rand() % derived.n_targets;
        for (i = 0; i < n; ++i) {
            while (c->u.nonrep.ch_targets[i] == t) {
                t = (t + 1) % derived.n_targets;
                i = 0;
            }
        }
        c->u.nonrep.ch_targets[n] = t;
        fprintf(log_f,",%d",t);
    }
    fprintf(log_f,"\n");
}

static void insert_next_chunk_put_ready (const chunkput_t *cp,
                                         tick_t insert_time)

// Generate thenext Chunk Put Ready event

{
    chunk_put_ready_t cpr;
    
    assert(cp);
    
    cpr.event.create_time = now;
    cpr.event.tllist.time = insert_time;
    
    if (insert_time <= now) insert_time = now + 1;
    
    cpr.event.type = CHUNK_PUT_READY;
    cpr.cp = (chunk_put_handle_t)cp;
        
    insert_event(cpr);
}

// add time for delay of unsolicited packet size

#define MINIMUM_UDPV6_BYTES 66
#define CHUNK_PUT_REQUEST_BYTES (MINIMUM_UDPV6_BYTES+200)

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

void handle_chunk_put_ready (const event_t *e)
{
    const chunk_put_ready_t *cpr = (const chunk_put_ready_t *)e;
    chunkput_t *cp = (chunkput_t *)cpr->cp;
    rep_chunk_put_request_received_t cprr;
    
    assert (cp);
    assert(!cp->mbz);
    
    if (replicast) {
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
    else {
        select_nonrep_targets(cp);
        next_tcp_replica_xmit(cp,e->tllist.time);
    }
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
    unsigned b0target = bids[0].target_num;
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

static  void select_replicast_targets (chunk_put_handle_t cp,
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
    }
    fprintf(bid_f,",index,%d,MaxQ,%d\n",m,max_qdepth);
    if (max_qdepth > MAX_QDEPTH) max_qdepth = MAX_QDEPTH;
    ++track.qdepth_tally[max_qdepth];
    ++track.n_qdepth_tally;
    track.qdepth_total += max_qdepth;
    if (max_qdepth > track.max_qdepth) track.max_qdepth = max_qdepth;
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
    assert(replicast);
    
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
    if ((new_cp = next_cp()) != NULL)
        insert_next_chunk_put_ready(new_cp,next_chunk_time);
}

static void remove_tcp_reception_target (chunkput_t *cp,unsigned target_num)

// this is a sanity checking diagnostic.
// It does notcontribute to the final results.

{
    unsigned  *p;
    
    for (p = cp->u.nonrep.ch_targets;
         p != cp->u.nonrep.ch_targets+config.n_replicas;
         ++p)
    {
        if (*p == target_num) {
            *p = 0;
            return;
        }
    }
}

void handle_tcp_reception_ack (const event_t *e)

// Handle the TCP reception ack, which occurs before the REPLICA_ACK
// The REPLICA_ACK occurs after the chunk replica is written.
// The tcp ack is sent once the chunk has been received.

{
    const tcp_reception_ack_t *tra = (const tcp_reception_ack_t *)e;
    chunkput_t *cp = (chunkput_t *)tra->cp;
    chunkput_t *new_cp;
    tick_t next_tcp_time;
 
    remove_tcp_reception_target(cp,tra->target_num);
    
    if (tra->max_ongoing_rx > cp->u.nonrep.max_ongoing_rx)
        cp->u.nonrep.max_ongoing_rx  = tra->max_ongoing_rx;
    next_tcp_time = e->tllist.time;
    next_tcp_time -= 3*config.cluster_trip_time;
    if (next_tcp_time <= now) next_tcp_time = now+1;
    if (++cp->u.nonrep.acked < config.n_replicas)
        next_tcp_replica_xmit(cp,next_tcp_time);
    else if ((new_cp = next_cp()) != NULL)
        insert_next_chunk_put_ready(new_cp,next_tcp_time);
}

void handle_replica_put_ack (const event_t *e)

// Simulate storage layer ack of a specific replica put being delivered to the
// gateway that originated the transaction.
{
    replica_put_ack_t *rpa = (replica_put_ack_t *)e;
    chunkput_t *cp = (chunkput_t *)rpa->cp;
    chunk_put_ack_t cpa;

    assert(cp);
    assert(!cp->mbz);
    assert(cp->sig == 0xABCD);
    assert(cp->seqnum);
    assert(rpa->target_num < derived.n_targets);
    assert(cp->replicas_unacked);
    assert(cp->replicas_unacked <= config.n_replicas);
    
    assert(rpa->write_qdepth >= 0);
    assert(cp->write_qdepth >= 0);
    if (rpa->write_qdepth > cp->write_qdepth)
        cp->write_qdepth = rpa->write_qdepth;
    
    if (!--cp->replicas_unacked) {
        cpa.event.create_time = e->tllist.time;
        cpa.event.tllist.time = e->tllist.time + 1;
        cpa.event.type = CHUNK_PUT_ACK;
        cpa.write_qdepth = cp->write_qdepth;
        cpa.cp = rpa->cp;

        insert_event(cpa);
    }
}

void handle_chunk_put_ack (const event_t *e)

// Simulate handling ack of entire chunk put
// return true if this chunk was trcked for statistics

{
    const chunk_put_ack_t *cpa = (const chunk_put_ack_t *)e;
    chunkput_t *cp;
    tick_t duration;

    unsigned long n_pending;
    char *tag = replicast ? "replicast" : "non";
    
    assert(e);
    assert(cpa->write_qdepth >= 0);

    cp = (chunkput_t *)cpa->cp;
    cp->done = e->tllist.time;
    assert(cp->seqnum);
    assert(cp->sig == 0xABCD);
    duration = cp->done - cp->started;

    assert(cp->write_qdepth >= 0);

    if (!config.terse) {
        fprintf(log_f,
                "0x%lx,%s Completion,%d,duration msec,%04.3f write_qdepth %d\n",
                e->tllist.time,tag,cp->seqnum,
                ((float)duration)/(10*1024*1024),cp->write_qdepth);
    }
    if (duration < track.min_duration) track.min_duration = duration;
    if (duration > track.max_duration) track.max_duration = duration;
    track.total_duration += duration;
    ++track.n_completions;
    n_pending = track.n_initiated - track.n_completions;
    if (!config.terse) {
        fprintf(log_f,"%s,chunk_ack:n_completions,%lu,n_initiated,%lu",
                tag,track.n_completions,track.n_initiated);
        fprintf(log_f,",pending,%lu,per target %04.3f\n",
            n_pending,((float)n_pending)/derived.n_targets);
    }
    if (cp->write_qdepth > MAX_WRITE_QDEPTH)
        cp->write_qdepth = MAX_WRITE_QDEPTH;
    if (!replicast) {
        ++track.qdepth_tally[cp->u.nonrep.max_ongoing_rx];
        ++track.n_qdepth_tally;
        track.qdepth_total += cp->u.nonrep.max_ongoing_rx;
        if (cp->u.nonrep.max_ongoing_rx > track.max_qdepth)
            track.max_qdepth = cp->u.nonrep.max_ongoing_rx;
    }
    ++track.write_qdepth_tally[cp->write_qdepth];
    ++track.n_write_qdepth_tally;
    track.write_dqepth_total += cp->write_qdepth;
    if (cp->write_qdepth > track.max_write_qdepth)
        track.max_write_qdepth = cp->write_qdepth;
    
    assert(!cp->replicas_unacked);
    memset(cp,0xFE,sizeof *cp);
    free(cp);
    assert(!track.mbz);
}

void report_duration_stats (void)

// report and clear the accumulated duratin and qdepth stats

{
    float avg_ticks,min_x,max_x,mbs,msecs,chunks_per_t;
    signed long m;
    unsigned long total_write;
    unsigned n;
    
    const float ticks_per_ms = 10L*1024*1024*1024/(float)1000;

    printf("\rPerformance results:");
    if (track.n_completions) {
        msecs = ((double_t)now)/ticks_per_ms;
        printf("\nTotal unique chunks: initiated %lu completed %lu ",
               track.n_initiated,track.n_completions);
        printf("execution-time %.1f ms\n",msecs);
        avg_ticks = (float)track.total_duration/(float)track.n_completions;
        min_x = (float)track.min_duration/avg_ticks;
        max_x = ((float)track.max_duration)/avg_ticks;
        printf("Chunk write latency (ms): min %3.2f (%.2f * avg) average %f ",
               ((float)track.min_duration)/ticks_per_ms,min_x,
               avg_ticks/ticks_per_ms);
        printf("max %3.2f (%.2f * avg)\n",
               ((float)track.max_duration)/ticks_per_ms,max_x);
        total_write = (unsigned long)track.n_completions * config.chunk_size *
            config.n_replicas;
        mbs = ((float)total_write)/(1024*1024) / derived.n_targets;
        chunks_per_t =
            (float)track.n_completions *
            ((float)config.n_replicas)/derived.n_targets;
        printf("\nAverage written per target: %6.2fMB   or %4.1f chunk-replicas\n",
               mbs, chunks_per_t);
        printf("Average target throughput:  %6.2fMB/s or %4.1f chunk-replicas/s\n",
               mbs*1000/msecs, chunks_per_t*1000/msecs);
        mbs = ((float)total_write)/(1024*1024) / config.n_gateways;
        chunks_per_t = (float)track.n_completions *
                        ((float)config.n_replicas)/config.n_gateways;
        printf("Average gateway sent:%6.2fMB or %4.1f chunks\n",mbs,
               chunks_per_t);
        printf("Average gateway throughput %6.2fMB/s\n",mbs*1000/msecs);
    
        fprintf(log_f,"\nInbound queue depth distribution:\n");
        for (n=0,m = track.n_qdepth_tally/2;n <= track.max_qdepth;++n) {
            fprintf(log_f,"([%d]:%d",n,track.qdepth_tally[n]);
            if (m > 0) {
                m -= track.qdepth_tally[n];
                if (m <=0) fprintf(log_f,"<-- Median");
            }
            fprintf (log_f,"\n");
        }
        fprintf(log_f,"Mean Average: %3.2f\n",
               ((float)track.qdepth_total)/track.n_qdepth_tally);
        fprintf(log_f,"\nWrite queue depth distribution:\n");
        for (n=0,m = track.n_write_qdepth_tally/2;
             n <= track.max_write_qdepth;
             ++n)
        {
            fprintf(log_f,"[%d]:%d",n,track.write_qdepth_tally[n]);
            if (m > 0) {
                m -= track.write_qdepth_tally[n];
                if (m <=0) fprintf(log_f,"<-- Median");
            }
            fprintf (log_f,"\n");
        }
    }
    fprintf(log_f,"Mean Average: %3.2f\n",
           ((float)track.write_dqepth_total)/track.n_write_qdepth_tally);
}

unsigned chunk_seq (chunk_put_handle_t cph)
{
    const chunkput_t *cp = (const chunkput_t *)cph;
    
    assert(cp->sig == 0xABCD);
    assert(cp->seqnum);
    return cp->seqnum;
}

unsigned chunk_gateway (chunk_put_handle_t cph)
{
    const chunkput_t *cp = (const chunkput_t *)cph;
    
    assert(cp->sig == 0xABCD);
    assert(cp->seqnum);
    return cp->gateway;
}

