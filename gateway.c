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

typedef struct chunkput {
    unsigned sig;               // must be 0xABCD
    unsigned count;             // sequence # (starting at 1) for all chunks
                                // put as part of this simulation
    unsigned ng;                // Chunk was assigned to this Negotiating Group
    unsigned remaining_chunks;  // # of chunkputs after this one for same object
    tick_t   started;           // When processing of this chunk started
    tick_t   done;              // When processing of this chunk completed
    unsigned responses_uncollected; // Chunk Put responses still to be collected
    bid_t    bids[MAX_TARGETS_PER_NG];    // collected bid response
                                    // once rendezvous transfer is scheduled it
                                    // is stored in bids[0].
    unsigned nbids;                 // # of bids collected
    unsigned replicas_unacked;  // Number of replicas not yet acked
    unsigned mbz;               // must be zero
} chunkput_t;

unsigned n_chunkputs = 0;

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
    cp->count = ++n_chunkputs;
    cp->started = e->tllist.time;
    cp->remaining_chunks = opr->n_chunks - 1;
    cp->replicas_unacked = config.n_replicas;
    cp->responses_uncollected = 0;
    if (replicast) {
        cpr.event.type = REP_CHUNK_PUT_READY;
        cp->ng = rand() % config.n_negotiating_groups;
        insert_event (cpr);
    }
    else {
        // select N_REPLICAS distinct targets
        // schedule each tcp_xmit_received on each target
        //      serial method: each xmit begins when prior completes
        //      daisy-chain: each begins after delay from prior
        //          Delay is 2 frames + outbound queueing delay
        // Note that the Chunk serialization is unchanged
        
        /* TODO: handle TCP/consistent-hash version
         * When will prior transmits be complete?
         * It is ok to transmit the next chunk at that time.
         * It is not necessary for the Chunk Put to be acked
         */
    }
}

static void put_next_chunk_request (const chunkput_t *prior_chunk,tick_t now)
{
    chunkput_t *cp = (chunkput_t *)calloc(1,sizeof(chunkput_t));

    rep_chunk_put_ready_t cpr;
    
    assert(prior_chunk);
    assert(prior_chunk->remaining_chunks);
    assert(!prior_chunk->mbz);
    assert(cp);
    assert(!cp->mbz);
 
    cp->sig = 0xABCD;
    cp->count = ++n_chunkputs;
    cp->started = cpr.event.create_time = now;
    cp->ng = rand() % config.n_negotiating_groups;
    cp->replicas_unacked = config.n_replicas;
    cp->remaining_chunks = prior_chunk->remaining_chunks - 1;
    cp->nbids = 0;
    cp->responses_uncollected = 0;
    cpr.event.create_time = now;
    cpr.event.tllist.time = now + 1;
    cpr.event.type = REP_CHUNK_PUT_READY;
    cpr.cp = (chunk_put_handle_t)cp;
    
    insert_event(cpr);
}

// add time for delay of unsolicited packet size

void handle_chunk_put_ready (const event_t *e)
{
    const rep_chunk_put_ready_t *cpp = (const rep_chunk_put_ready_t *)e;
    rep_chunk_put_request_received_t new_event;
    chunkput_t *p = (chunkput_t *)cpp->cp;
    
    assert (p);
    assert(!p->mbz);
    new_event.event.create_time = e->tllist.time;
    new_event.event.tllist.time   = e->tllist.time + CLUSTER_TRIP_TIME;
    new_event.event.type = REP_CHUNK_PUT_REQUEST_RECEIVED;
    new_event.cp = (chunk_put_handle_t)p;
    p->responses_uncollected = config.n_targets_per_ng;

    /* for each Target in randomly selected negotiating group.
     * the Targets are assigned round-robin to Negotiating Groups.
     *
     * Actual Negotiating Groups are selected based on cryptographic hash
     * of the payload (for payload chunks) or the object name (for metadata)
     */
    for (new_event.target_num = p->ng = rand() % config.n_negotiating_groups;
         new_event.target_num < derived.n_targets;
         new_event.target_num += config.n_negotiating_groups)
    {
        insert_event(new_event);
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
    *lim = window_start + derived.chunk_xmit_duration;
    return window_lim >= *lim;
}

#define MAX_QDEPTH 10
static unsigned qdepth_tally[MAX_QDEPTH+1] = {0};

static  void select_targets (chunk_put_handle_t cp,
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
    for (n = 0; n != nbids; ++n)
        fprintf(bid_f,"BIDS:CP #,%d,Start,%ld,Lim,%ld,Qdepth,%d,Target:%d\n",
                c->count,bids[n].start,bids[n].lim,bids[n].queue_depth,
                bids[n].target_num);
    for (n = 0; n + config.n_replicas <= nbids; ++n) {
        if (acceptable_bid_set(bids+n,&start,&lim)) {
            bids[0].start = start;
            bids[0].lim = lim;
            fprintf(bid_f,"BIDS:CP #,%d,Now,%ld,Accepted,%ld,%ld,TARGET",
                    c->count,now,start,lim);
            for (m = 0,max_qdepth = 0; m != config.n_replicas; ++m) {
                if (bids[n+m].queue_depth > max_qdepth)
                    max_qdepth = bids[n+m].queue_depth;
                accepted_target[m] = bids[n+m].target_num;
                fprintf(bid_f,",%d",accepted_target[m]);
            }
            fprintf(bid_f,",MaxQ,%d\n",max_qdepth);
            if (max_qdepth > MAX_QDEPTH) max_qdepth = MAX_QDEPTH;
            ++qdepth_tally[max_qdepth];
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
    
    assert (p);
    assert(!p->mbz);
    assert(p->sig == 0xABCD);
    assert(p->count);
    assert (0 < p->responses_uncollected);
    assert (p->responses_uncollected <= config.n_targets_per_ng);
    
    save_bid (p->bids,&p->nbids,cpr);
    if (--p->responses_uncollected) return;
    
    accept_event.event.create_time = e->tllist.time;
    accept_event.event.tllist.time = e->tllist.time + CLUSTER_TRIP_TIME;
    accept_event.event.type = REP_CHUNK_PUT_ACCEPT_RECEIVED;
    accept_event.cp = cpr->cp;
    
    select_targets (cpr->cp,p->nbids,p->bids,accept_event.accepted_target);
    accept_event.window_start = p->bids[0].start;
    accept_event.window_lim = p->bids[0].lim;

    rendezvous_xfer_event.event.create_time = accept_event.event.create_time;
    rendezvous_xfer_event.event.tllist.time = accept_event.window_lim;
    rendezvous_xfer_event.event.type = REP_RENDEZVOUS_XFER_RECEIVED;
    rendezvous_xfer_event.cp = accept_event.cp;
    
    for (accept_event.target_num = p->ng;
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
    if (p->remaining_chunks)
        put_next_chunk_request(p,rendezvous_xfer_event.event.tllist.time);
}

void handle_replica_put_ack (const event_t *e)
{
    replica_put_ack_t *rpa = (replica_put_ack_t *)e;
    chunkput_t *c = (chunkput_t *)rpa->cp;
    chunk_put_ack_t new_event;

    assert(c);
    assert(!c->mbz);
    assert(c->sig == 0xABCD);
    assert(c->count);
    assert(rpa->target_num < derived.n_targets);
    assert(0 < c->replicas_unacked && c->replicas_unacked <= config.n_replicas);
    
    if (!--c->replicas_unacked) {
        new_event.event.create_time = e->tllist.time;
        new_event.event.tllist.time = e->tllist.time + 1;
        new_event.event.type = CHUNK_PUT_ACK;
        new_event.cp = rpa->cp;

        insert_event(new_event);
    }
}


static tick_t min_duration = (tick_t)~0;
static tick_t max_duration = 0;
static tick_t total_duration = 0;
static unsigned long n_completions = 0;
static unsigned this_mbz = 0;

#define DURATION_SAMPLE_SIZE 100
static tick_t last100_durations[DURATION_SAMPLE_SIZE];
static int    l100_idx = 0;
static tick_t running_avg_duration = 0;
static float  running_avg_stdev = 0;

bool handle_chunk_put_ack (const event_t *e)
{
    const chunk_put_ack_t *cpa = (const chunk_put_ack_t *)e;
    chunkput_t *cp;
    tick_t duration;
    bool was_tracked;
    
    assert(e);
    cp = (chunkput_t *)cpa->cp;
    cp->done = e->tllist.time;
    assert(cp->count);
    assert(cp->sig == 0xABCD);
    duration = cp->done - cp->started;
    if ((was_tracked = cp->count <= derived.n_tracked_puts) != 0) {
        fprintf(log_f,"Completion,%d,duration msec,%0.3f\n",cp->count,
                ((float)duration)/(10*1024*1024));
        if (duration < min_duration) min_duration = duration;
        if (duration > max_duration) max_duration = duration;
        total_duration += duration;
        ++n_completions;

        if (l100_idx < DURATION_SAMPLE_SIZE) {
            last100_durations[l100_idx] = duration;
        }
        l100_idx++;
        if (l100_idx == DURATION_SAMPLE_SIZE) {
            int sample_average = 0;
            float sample_stdev = 0;
            int i;
            for (i = 0; i < DURATION_SAMPLE_SIZE; i++) {
                sample_average += last100_durations[i];
            }
            sample_average /= DURATION_SAMPLE_SIZE;
            running_avg_duration =
                (4 * running_avg_duration + 6 * sample_average) / 10;

            for (i = 0; i < DURATION_SAMPLE_SIZE; i++) {
                sample_stdev +=
                    (last100_durations[i] - sample_average) *
                    (last100_durations[i] - sample_average);
            }
            sample_stdev /= (DURATION_SAMPLE_SIZE - 1);
            sample_stdev = sqrtf(sample_stdev);

            running_avg_stdev = (4 * running_avg_stdev + 6 * sample_stdev) / 10;

            memset(last100_durations, 0, DURATION_SAMPLE_SIZE * sizeof(tick_t));
            l100_idx = 0;
        }
    }
    cp->sig = 0xDEAD;
    free(cp);
    --n_chunkputs;

    assert(!this_mbz);
    return was_tracked;
}

void report_duration_stats (void)
{
    float avg_x,max_x;
    tick_t avg;
    unsigned n;
    
    printf("\n\n# completions %ld\n",n_completions);
    if (n_completions) {
        avg = divup(total_duration,n_completions);
        avg_x = ((float)avg)/min_duration;
        max_x = ((float)max_duration)/min_duration;
        printf("min msecs %0.3f average %0.3f (x%f) max %0.3f (x%f)\n",
               ((float)min_duration)/(10*1024*1024),
               ((float)avg)/(10*1024*1024),avg_x,
               ((float)max_duration)/(10*1024*10240),max_x);
        printf("running-average-duration %ld\n", running_avg_duration);
        printf("running-average-stdev %f\n", running_avg_stdev);
    }
    printf("Queue depth distribution:\n");
    for (n=0;n < MAX_QDEPTH+1;++n)
        printf("([%d]:%d\n",n,qdepth_tally[n]);
}

unsigned chunk_seq (chunk_put_handle_t cp)
{
    const chunkput_t *p = (const chunkput_t *)cp;
    
    assert(p->sig == 0xABCD);
    assert(p->count);
    return p->count;
}

