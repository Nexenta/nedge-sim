//
//  gateway.c
//  StorageClusterSim
//
//  Created by cait on 5/13/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#include "storage_cluster_sim.h"
#include <math.h>

// Where is #define N_GATEWAYS? We don't need it.
//
// Gateway.c holds the handler routines to simulate Gateway functionality.
//
// We do not model *which* gateway is performing any given task because it is
// not relevant to the simulation. With both Replicast and Consistent Hashing
// it is possible to allocate enough gateway nodes so that there is no need
// for transactions to wait for gateway processing power. More importantly,
// even if you did not allocate enough resources the result would be the same.
//
// The purpose of this simulation is demonstrate the differences in how Tarets
// are chosen and how they are communicated with. The Gateways are only
// different in how they talk to the Targets.
//

typedef struct bid {
    tick_t  start;
    tick_t  lim;
    tick_t  estimated_ack;
    unsigned target_num;
    unsigned queue_depth;
} bid_t;
//
// A bid by 'target_num' to store a chunk within the time range 'start'..'lim'
//

// Chunkput Tracking
//
// Information about a pending Chunk Put is accessible only on the Gateway
// So the struct is only defined in this file. An unsigned long holds the
// pointer for the target event handlers.
//
// Each chunkput_t is allocated by the gateway, and then freed when it is done.

#define MAX_TARGETS_PER_NG 20

typedef struct chunkput_replicast {
    // replicast-specific ortion of chunkput_t
    unsigned ng;     // Chunk has been assigned to this NG
    bid_t    bids[MAX_TARGETS_PER_NG];
        // collected bid response
        // once rendezvous transfer is scheduled it is stored in bids[0].
    unsigned nbids; // # of bids collected
    unsigned responses_uncollected; // Chunk Put responses still to be collected
} chunkput_replicast_t;

typedef struct chunkput_nonreplicast {
    // non-repicast specfici portion of chunkput_t
    unsigned ch_targets[MAX_REPLICAS]; // selected targets
    unsigned repnum;                    // # of replicas previously generated.
    unsigned acked;
    unsigned max_ongoing_rx;    // maximum n_ongoing_receptions for any target
} chunkput_nonreplicast_t;

typedef struct chunkput {       // track gateway-specific info about a chunkput
    unsigned sig;               // must be 0xABCD
    unsigned seqnum;             // sequence # (starting at 1) for all chunks
                                // put as part of this simulation
    unsigned remaining_chunks;  // # of chunkputs after this one for same object
    tick_t   started;           // When processing of this chunk started
    tick_t   done;              // When processing of this chunk completed
    unsigned replicas_unacked;  // Number of replicas not yet acked
    unsigned write_qdepth;      // Maximum write queue depth encountered for
                                // this chunk put
    union chunkput_u {
        chunkput_replicast_t replicast;
        chunkput_nonreplicast_t nonrep;
    } u;
    unsigned mbz;               // must be zero
} chunkput_t;

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
#define TCP_CHUNK_SETUP_BYTES (4*MINIMUM_TCPV6_BYTES+200)
    // 4 packets for TCP connectino setup plus minimal pre-transfer data
    // The cluster_trip_time must still be added to this.

static void next_tcp_xmit (chunkput_t *cp,tick_t time_now)

// Schedule the next TCP transmit start after the previous tcp transmit for
// the same object has completed

{
    tcp_xmit_received_t txr;
    unsigned r;
    
    if (cp->replicas_unacked) {
        txr.event.create_time = time_now;
        txr.event.tllist.time = time_now + config.cluster_trip_time*4 +
                                TCP_CHUNK_SETUP_BYTES*8;
        txr.event.type = TCP_XMIT_RECEIVED;
        txr.cp = (chunk_put_handle_t)cp;
        r = cp->u.nonrep.repnum++;
        txr.target_num = cp->u.nonrep.ch_targets[r];
        insert_event(txr);
    }
}

static unsigned n_chunkputs = 0;

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

void handle_object_put_ready (const event_t *e)

// replicast: create the first chunk_put_ready for this object, post it
// once it is scheduled the next chunk_put_request can be sent.

{
    const object_put_ready_t *opr = (const object_put_ready_t *)e;
    rep_chunk_put_ready_t cpr;
    chunkput_t *cp = (chunkput_t *)calloc(1,sizeof(chunkput_t));
    
    assert(cp);
    assert(!cp->mbz);

    cpr.event.create_time = e->tllist.time;
    cpr.event.tllist.time = e->tllist.time + 1;
    cpr.cp = (chunk_put_handle_t)cp;
    cp->sig = 0xABCD;

    cp->seqnum = ++n_chunkputs;
    ++track.n_initiated;
    cp->started = e->tllist.time;
    cp->remaining_chunks = opr->n_chunks - 1;
    cp->replicas_unacked = config.n_replicas;
    assert(cp->replicas_unacked);
    cpr.event.type = REP_CHUNK_PUT_READY;

    insert_event (cpr);
}

static void put_next_chunk_request (const chunkput_t *prior_chunk,tick_t now)

// Generate thenext Chunk Put Ready event for the current object

{
    chunkput_t *cp = (chunkput_t *)calloc(1,sizeof(chunkput_t));
    rep_chunk_put_ready_t cpr;
    
    assert(prior_chunk);
    assert(prior_chunk->remaining_chunks);
    assert(!prior_chunk->mbz);
    assert(cp);
    assert(!cp->mbz);
 
    cp->sig = 0xABCD;
    cp->seqnum = ++n_chunkputs;
    ++track.n_initiated;
    cp->started = cpr.event.create_time = now;
    cp->replicas_unacked = config.n_replicas;
    assert(cp->replicas_unacked);
    cp->remaining_chunks = prior_chunk->remaining_chunks - 1;

    cpr.event.create_time = now;
    cpr.event.tllist.time = now + 1;
    cpr.event.type = REP_CHUNK_PUT_READY;
    cpr.cp = (chunk_put_handle_t)cp;
    if (replicast)
        cp->u.replicast.ng = rand() % config.n_negotiating_groups;

    insert_event(cpr);
}

// add time for delay of unsolicited packet size

#define MINIMUM_UDPV6_BYTES 66
#define CHUNK_PUT_REQUEST_BYTES (MINIMUM_UDPV6_BYTES+200)

void handle_chunk_put_ready (const event_t *e)
{
    const rep_chunk_put_ready_t *cpp = (const rep_chunk_put_ready_t *)e;
    rep_chunk_put_request_received_t new_event;
    chunkput_t *p = (chunkput_t *)cpp->cp;
    
    assert (p);
    assert(!p->mbz);
    new_event.event.create_time = e->tllist.time;
    new_event.event.tllist.time   = e->tllist.time + config.cluster_trip_time +
                                    CHUNK_PUT_REQUEST_BYTES*8;
    new_event.event.type = REP_CHUNK_PUT_REQUEST_RECEIVED;
    new_event.cp = (chunk_put_handle_t)p;
    if (replicast) {
        p->u.replicast.responses_uncollected = config.n_targets_per_ng;

        /* for each Target in randomly selected negotiating group.
         * the Targets are assigned round-robin to Negotiating Groups.
         *
         * Actual Negotiating Groups are selected based on cryptographic hash
         * of the payload (for payload chunks) or the object name (for metadata)
         */
        for (new_event.target_num = p->u.replicast.ng =
                rand() % config.n_negotiating_groups;
             new_event.target_num < derived.n_targets;
             new_event.target_num += config.n_negotiating_groups)
        {
            insert_event(new_event);
        }
    }
    else {
        select_nonrep_targets(p);
        next_tcp_xmit(p,e->tllist.time);
    }
}

static void save_bid (bid_t *bids,
                      unsigned *nbids,
                      const rep_chunk_put_response_received_t *cpr)
//
// save the bid within 'cpr' in bids[n*bids], then increment *nbids
// verify that *nbids <= N_TARGETS_PER_NG
//
{
    bid_t *b;
    assert(cpr);
    assert(cpr->bid_start < cpr->bid_lim);
    assert(cpr->target_num < derived.n_targets);
    assert (nbids);
    assert (*nbids <= config.n_targets_per_ng);
    
    b = bids + *nbids;
    b->start = cpr->bid_start;
    b->lim = cpr->bid_lim;
    b->estimated_ack = cpr->estimated_ack;
    b->target_num = cpr->target_num;
    b->queue_depth = cpr->qdepth;
    ++*nbids;
}

static int bid_compare (const void *a,const void *b)
//
// Compare two bids for qsort()
//
{
    const bid_t *ba = a;
    const bid_t *bb = b;
    
    return ba->start < bb->start ? -1
        : ba->start == bb->start ? 0
        : 1;
}

static bool acceptable_bid_set (const bid_t *bids,
                                tick_t *start,
                                tick_t *lim)

// find the overlapping window for 'bids[0] thru bids[N_REPLICAS-1]
// if this is <= duration then
//      return true
//      set *start to base of the overlapping window
//      set *lim to duration ticks later
// otherwise
//      return false
//      *start and *lim may have been modified

{
    const bid_t *b;
    tick_t window_start = bids[0].start;
    tick_t window_lim = bids[0].lim;
    
    for (b = bids+1; b < bids+config.n_replicas;++b) {
        assert(b->start < b->lim);
        if (b->start > window_start) window_start = b->start;
        if (b->lim < window_lim) window_lim = b->lim;
    }
    *start = window_start;
    *lim = window_start + derived.chunk_udp_xmit_duration;
    return window_lim >= *lim;
}

static bool in_accepted_list(unsigned t,unsigned *accepted)
{
    unsigned n;
    
    for (n = 0; n!= config.n_replicas; ++n) {
        if (accepted[n] == t)
            return true;
    }
    return false;
}

static  void scan_for_better_targets (unsigned nbids,
                                       bid_t *bids,
                                       unsigned *accepted_target)
{
    unsigned n;
    tick_t best_estimated_ack;
    tick_t best_unaccepted_ack;
    tick_t *compare;
    
    best_unaccepted_ack = best_estimated_ack = ~0L;
    for (n = 0; n < nbids; ++n) {
        compare = in_accepted_list(bids[n].target_num,accepted_target)
            ? &best_estimated_ack
            : &best_unaccepted_ack;
        if (best_estimated_ack < *compare)
            *compare = bids[n].estimated_ack;
    }
    if (best_unaccepted_ack < best_estimated_ack)
        fprintf(bid_f,"Best write ack not selected\n");
}

// determine latest estimated-ack in accepted set
// scan entire set to see if there is a better estimated_ack
// for now just note it in bid_f
static  void select_replicast_targets (chunk_put_handle_t cp,
                             unsigned nbids,
                             bid_t *bids,
                             unsigned *accepted_target)

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
    
    assert (nbids <= config.n_targets_per_ng);
    
    qsort(bids,nbids,sizeof(bid_t),bid_compare);
    for (n = 0; n != nbids; ++n) {
        fprintf(bid_f,"BIDS:CP #,%d,Start,%ld,Lim,%ld,Qdepth,%d,Target:%d",
                c->seqnum,bids[n].start,bids[n].lim,bids[n].queue_depth,
                bids[n].target_num);
        fprintf(bid_f,",EstAck,0x%lx\n",bids[n].estimated_ack);
    }
    for (n = 0; n + config.n_replicas <= nbids; ++n) {
        if (acceptable_bid_set(bids+n,&start,&lim)) {
            bids[0].start = start;
            bids[0].lim = lim;
            fprintf(bid_f,"BIDS:CP #,%d,Now,%ld,Accepted,%ld,%ld,TARGET",
                    c->seqnum,now,start,lim);
            for (m = 0,max_qdepth = 0; m != config.n_replicas; ++m) {
                if (bids[n+m].queue_depth > max_qdepth)
                    max_qdepth = bids[n+m].queue_depth;
                accepted_target[m] = bids[n+m].target_num;
                fprintf(bid_f,",%d",accepted_target[m]);
            }
            fprintf(bid_f,",MaxQ,%d\n",max_qdepth);
            if (max_qdepth > MAX_QDEPTH) max_qdepth = MAX_QDEPTH;
            ++track.qdepth_tally[max_qdepth];
            ++track.n_qdepth_tally;
            track.qdepth_total += max_qdepth;
            if (max_qdepth > track.max_qdepth) track.max_qdepth = max_qdepth;
            scan_for_better_targets(nbids,bids,accepted_target);
            return;
        }
    }
    // Because every member of the negotiating group MUST bid we should
    // always find an acceptable set. A real storage cluster would re-bid
    // starting here to deal with temporary disruptions caused by temporary
    // loss of network connectivity.
    assert(false);
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
    const rep_chunk_put_response_received_t *cpr =
        (const rep_chunk_put_response_received_t *)e;
    rep_chunk_put_accept_t accept_event;
    rep_rendezvous_xfer_received_t rendezvous_xfer_event;
    chunkput_t *p = (chunkput_t *)cpr->cp;
    tick_t next_chunk_time;
    
    assert(p);
    assert(!p->mbz);
    assert(p->sig == 0xABCD);
    assert(p->seqnum);
    assert(0 < p->u.replicast.responses_uncollected);
    assert(p->u.replicast.responses_uncollected <= config.n_targets_per_ng);
    assert(replicast);
    
    save_bid (p->u.replicast.bids,&p->u.replicast.nbids,cpr);
    if (--p->u.replicast.responses_uncollected) return;
    
    accept_event.event.create_time = e->tllist.time;
    accept_event.event.tllist.time = e->tllist.time + config.cluster_trip_time;
    accept_event.event.type = REP_CHUNK_PUT_ACCEPT_RECEIVED;
    accept_event.cp = cpr->cp;

    memset(&accept_event.accepted_target[0],0,
           sizeof accept_event.accepted_target);
    
    select_replicast_targets (cpr->cp,p->u.replicast.nbids,p->u.replicast.bids,
                    accept_event.accepted_target);
    accept_event.window_start = p->u.replicast.bids[0].start;
    accept_event.window_lim = p->u.replicast.bids[0].lim;

    rendezvous_xfer_event.event.create_time = accept_event.event.create_time;
    rendezvous_xfer_event.event.tllist.time = accept_event.window_lim;
    assert(accept_event.window_lim > accept_event.event.create_time);
    rendezvous_xfer_event.event.type = REP_RENDEZVOUS_XFER_RECEIVED;
    rendezvous_xfer_event.cp = accept_event.cp;
    
    for (accept_event.target_num = p->u.replicast.ng;
         accept_event.target_num < derived.n_targets;
         accept_event.target_num += config.n_negotiating_groups)
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
    if (p->remaining_chunks) {
        next_chunk_time = rendezvous_xfer_event.event.tllist.time;
        // adjust earlier by fastest negotiation time possible
        put_next_chunk_request(p,next_chunk_time);
    }
}

static void remove_tcp_reception_target (chunkput_t *c,unsigned target_num)

// this is a sanity checking diagnostic.
// It does notcontribute to the final results.

{
    unsigned  *p;
    
    for (p = c->u.nonrep.ch_targets;
         p != c->u.nonrep.ch_targets+config.n_replicas;
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
    chunkput_t *c = (chunkput_t *)tra->cp;
    tick_t next_tcp_time;
 
    remove_tcp_reception_target(c,tra->target_num);
    
    if (tra->max_ongoing_rx > c->u.nonrep.max_ongoing_rx)
        c->u.nonrep.max_ongoing_rx  = tra->max_ongoing_rx;
    next_tcp_time = e->tllist.time;
    // adjust so that connection setup overlaps with delivery
    if (++c->u.nonrep.acked < config.n_replicas)
        next_tcp_xmit(c,next_tcp_time);
    else if (c->remaining_chunks)
        put_next_chunk_request (c,next_tcp_time);
}

void handle_replica_put_ack (const event_t *e)

// Simulate storage layer ack of a specific replica put being delivered to the
// gateway that originated the transaction.
{
    replica_put_ack_t *rpa = (replica_put_ack_t *)e;
    chunkput_t *c = (chunkput_t *)rpa->cp;
    chunk_put_ack_t put_ack_event;

    assert(c);
    assert(!c->mbz);
    assert(c->sig == 0xABCD);
    assert(c->seqnum);
    assert(rpa->target_num < derived.n_targets);
    assert(c->replicas_unacked);
    assert(c->replicas_unacked <= config.n_replicas);
    
    if (rpa->write_qdepth > c->write_qdepth)
        c->write_qdepth = rpa->write_qdepth;
    
    if (!--c->replicas_unacked) {
        put_ack_event.event.create_time = e->tllist.time;
        put_ack_event.event.tllist.time = e->tllist.time + 1;
        put_ack_event.event.type = CHUNK_PUT_ACK;
        put_ack_event.cp = rpa->cp;

        insert_event(put_ack_event);
    }
}

bool handle_chunk_put_ack (const event_t *e)

// Simulate handling ack of entire chunk put
// return true if this chunk was trcked for statistics

{
    const chunk_put_ack_t *cpa = (const chunk_put_ack_t *)e;
    chunkput_t *cp;
    tick_t duration;
    bool was_tracked;
    unsigned long n_pending;
    char *tag = replicast ? "replicast" : "non";
    
    assert(e);
    cp = (chunkput_t *)cpa->cp;
    cp->done = e->tllist.time;
    assert(cp->seqnum);
    assert(cp->sig == 0xABCD);
    duration = cp->done - cp->started;
    was_tracked = cp->seqnum <= derived.n_tracked_puts;
    if (was_tracked) {
        fprintf(log_f,
                "0x%lx,%s Completion,%d,duration msec,%04.3f write_qdepth %d\n",
                e->tllist.time,tag,cp->seqnum,
                ((float)duration)/(10*1024*1024),cp->write_qdepth);
        if (duration < track.min_duration) track.min_duration = duration;
        if (duration > track.max_duration) track.max_duration = duration;
        track.total_duration += duration;
        ++track.n_completions;
        fprintf(log_f,"chunk_ack:n_completions,%lu,n_initiated,%lu",
                track.n_completions,track.n_initiated);
        n_pending = track.n_initiated - track.n_completions;
        fprintf(log_f,",pending,%lu,per target %04.3f\n",
                n_pending,((float)n_pending)/derived.n_targets);

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
    }
    assert(!cp->replicas_unacked);
    memset(cp,0xFE,sizeof *cp);
    free(cp);

    assert(!track.mbz);
    return was_tracked;
}

void report_duration_stats (void)

// report and clear the accumulated duratin and qdepth stats

{
    float avg,avg_x,max_x,mbs,msecs;
    signed long m;
    unsigned long total_write;
    unsigned n;
    
    if (track.n_completions) {
        msecs = ((double_t)now)/(10L*1024*1024*1024/1000);
        printf("n_initiated %lu n_completions %lu total time %6.3f msecs\n",
               track.n_initiated,track.n_completions,msecs);
        avg = ((float)track.total_duration)/track.n_completions;
        avg_x = avg/track.min_duration;
        max_x = ((float)track.max_duration)/track.min_duration;
        printf("min msecs %3.3f average %3.3f (x%f) max %3.3f (x%f)\n",
               ((float)track.min_duration)/(10L*1024*1024*1024/1000),
               avg/(10L*1024*1024*1024/1000),avg_x,
               ((float)track.max_duration)/(10L*1024*1024*1024/1000),max_x);
        total_write = (unsigned long)track.n_completions * config.chunk_size *
            config.n_replicas;
        mbs = ((float)total_write)/(1024*1024) / derived.n_targets;
        printf("Average Written per target %6.3f MBs\n",mbs);
        printf("MB/sec Rate per target %6.3f\n",mbs*1000/msecs);
    
        printf("\nInbound Queue depth distribution:\n");
        for (n=0,m = track.n_qdepth_tally/2;n <= track.max_qdepth;++n) {
            printf("([%d]:%d",n,track.qdepth_tally[n]);
            if (m > 0) {
                m -= track.qdepth_tally[n];
                if (m <=0) printf("<-- Median");
            }
            printf ("\n");
        }
        printf("Mean Average: %3.3f\n",
               ((float)track.qdepth_total)/track.n_qdepth_tally);
        printf("\nWrite Queue Depth distribution:\n");
        for (n=0,m = track.n_write_qdepth_tally/2;
             n <= track.max_write_qdepth;
             ++n)
        {
            printf("[%d]:%d",n,track.write_qdepth_tally[n]);
            if (m > 0) {
                m -= track.write_qdepth_tally[n];
                if (m <=0) printf("<-- Median");
            }
            printf ("\n");
        }
    }
    printf("Mean Average: %3.3f\n",
           ((float)track.write_dqepth_total)/track.n_write_qdepth_tally);
    memset(&track,0,sizeof(trackers_t));
    track.min_duration = ~0L;
}

unsigned chunk_seq (chunk_put_handle_t cp)
{
    const chunkput_t *p = (const chunkput_t *)cp;
    
    assert(p->sig == 0xABCD);
    assert(p->seqnum);
    return p->seqnum;
}

