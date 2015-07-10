
//
//  gateway.c
//  StorageClusterSim
//
//  Created by Caitlin Bestler on 7/9/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#include "storage_cluster_sim.h"
#include <math.h>

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
    unsigned gateway;           // which gateway?
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



void insert_next_put (unsigned gateway,tick_t insert_time)

// insert the next object_put_ready event for the current object

{
    gateway_ready_t new_put;
    
    new_put.event.tllist.time = new_put.event.create_time = insert_time;
    new_put.event.type = GATEWAY_READY;
    new_put.gateway = gateway;
    insert_event(new_put);
}

#define MINIMUM_TCPV6_BYTES 74
#define TCP_CHUNK_SETUP_BYTES (4*MINIMUM_TCPV6_BYTES+200)
    // 4 packets for TCP connectino setup plus minimal pre-transfer data
    // The cluster_trip_time must still be added to this.


static unsigned n_chunkputs = 0;

static chunkput_t *next_cp (unsigned gw)
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
    cp->gateway = gw;
    
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
        txr.event.tllist.time = time_now + config.cluster_trip_time*4 +
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

void handle_gateway_ready (const event_t *e)

// replicast: create the first chunk_put_ready for this object, post it
// once it is scheduled the next chunk_put_request can be sent.

{
    const gateway_ready_t *gr = (const gateway_ready_t *)e;
    chunk_put_ready_t cpr;
    chunkput_t *cp;
    
    cp = (chunkput_t *)calloc(1,sizeof(chunkput_t));
    assert(cp);
    assert(!cp->mbz);
    
    cpr.event.create_time = e->tllist.time;
    cpr.event.tllist.time = e->tllist.time + 1;
    cpr.cp = (chunk_put_handle_t)cp;
    cp->sig = 0xABCD;
    
    cp->seqnum = ++n_chunkputs;
    ++track.n_initiated;
    cp->started = e->tllist.time;
    cp->gateway = gr->gateway;
    cp->replicas_unacked = config.n_replicas;
    assert(cp->replicas_unacked);
    cpr.event.type = CHUNK_PUT_READY;
    
    insert_event (cpr);
}

static void insert_next_chunk_request (const chunkput_t *cp,tick_t time)

// Generate thenext Chunk Put Ready event for the current object

{
    chunk_put_ready_t cpr;
    
    assert(cp);
    assert(!cp->mbz);
    
    cpr.event.create_time = time;
    cpr.event.tllist.time = time + 1;
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
    rep_chunk_put_request_received_t cprr;
    chunkput_t *cp = (chunkput_t *)cpr->cp;
    gateway_t *gw = gateway + cp->gateway;
    
    assert (cp);
    assert(!cp->mbz);
    
    if (!gw->credit) {
        if (now < config.sim_duration  &&  !gw->pending_cp)
            gw->pending_cp = cp;
    }
    else {
        --gw->credit;

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
    chunkput_t *cp = (chunkput_t *)cpr->cp;
    chunkput_t *new_cp;
    gateway_t *gw = gateway + cp->gateway;
    tick_t next_chunk_time;
    
    assert(cp);
    assert(!cp->mbz);
    assert(cp->sig == 0xABCD);
    assert(cp->seqnum);
    assert(0 < cp->u.replicast.responses_uncollected);
    assert(cp->u.replicast.responses_uncollected <= config.n_targets_per_ng);
    assert(replicast);
    
    save_bid (cp->u.replicast.bids,&cp->u.replicast.nbids,cpr);
    if (--cp->u.replicast.responses_uncollected) return;
    
    accept_event.event.create_time = e->tllist.time;
    accept_event.event.tllist.time = e->tllist.time + config.cluster_trip_time;
    accept_event.event.type = REP_CHUNK_PUT_ACCEPT_RECEIVED;
    accept_event.cp = cpr->cp;

    memset(&accept_event.accepted_target[0],0,
           sizeof accept_event.accepted_target);
    
    select_replicast_targets (cpr->cp,cp->u.replicast.nbids,
                              cp->u.replicast.bids,
                              accept_event.accepted_target);
    accept_event.window_start = cp->u.replicast.bids[0].start;
    accept_event.window_lim = cp->u.replicast.bids[0].lim;

    rendezvous_xfer_event.event.create_time = accept_event.event.create_time;
    rendezvous_xfer_event.event.tllist.time = accept_event.window_lim;

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
    if (!gw->credit) {
        if (!gw->pending_cp) {
            gw->pending_cp = next_cp(cp->gateway);
        }
    }
    else {
        --gw->credit;
        next_chunk_time = rendezvous_xfer_event.event.tllist.time;
        next_chunk_time -= 3*config.cluster_trip_time;
        if (next_chunk_time <= now) next_chunk_time = now+1;
        if ((new_cp = next_cp(cp->gateway)) == NULL)
            ++gw->credit;
        else
            insert_next_chunk_request(new_cp,next_chunk_time);
    }
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
    unsigned gway = cp->gateway;
    gateway_t *gw = gateway + gway;
    tick_t next_tcp_time;
 
    remove_tcp_reception_target(cp,tra->target_num);
    
    if (tra->max_ongoing_rx > cp->u.nonrep.max_ongoing_rx)
        cp->u.nonrep.max_ongoing_rx  = tra->max_ongoing_rx;
    next_tcp_time = e->tllist.time;
    next_tcp_time -= 2*config.cluster_trip_time;
    if (next_tcp_time <= now) next_tcp_time = now+1;
    if (++cp->u.nonrep.acked < config.n_replicas)
        next_tcp_replica_xmit(cp,next_tcp_time);
    else if (gw->credit) {
        new_cp = next_cp(gway);
        if (new_cp != NULL) {
            --gw->credit;
            select_nonrep_targets(new_cp);
            next_tcp_replica_xmit(new_cp,next_tcp_time);
        }
    }
    else if (!gw->pending_cp) {
        new_cp = next_cp(gway);
        gw->pending_cp = new_cp;
    }
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
    
    assert(cp->write_qdepth <= MAX_WRITE_QDEPTH);
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
    gateway_t *gw;
    tick_t duration;

    unsigned long n_pending;
    char *tag = replicast ? "replicast" : "non";
    
    assert(e);
    cp = (chunkput_t *)cpa->cp;
    cp->done = e->tllist.time;
    assert(cp->seqnum);
    assert(cp->sig == 0xABCD);
    duration = cp->done - cp->started;

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
    
    gw = gateway + cp->gateway;
    
    assert(!cp->replicas_unacked);
    memset(cp,0xFE,sizeof *cp);
    free(cp);
    assert(!track.mbz);
    
    ++gw->credit;
    if ((cp = gw->pending_cp) != (void *)0) {
        gw->pending_cp = (void *)0;

        if (replicast)
            insert_next_chunk_request(cp,now+1);
        else {
            select_nonrep_targets(cp);
            next_tcp_replica_xmit(cp,now+1);
        }
    }
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

