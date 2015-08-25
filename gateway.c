
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

chunkput_t *next_cp (gateway_t *gateway)
{
    chunkput_t *cp;
    
    if (now > config.sim_duration) return (chunkput_t *)0;

    cp = (chunkput_t *)calloc(1,sizeof(chunkput_t));
    assert(cp);
    assert(!cp->mbz);
    
    cp->sig = 0xABCD;
    cp->seqnum = ++n_chunkputs;
    ++track.n_initiated;
    cp->replicas_unacked = config.n_replicas;
    cp->gateway = gateway;
    assert(cp->replicas_unacked);

    // only relevant for replicast protocols, but harmless otherwise
    cp->ng = rand() % config.n_negotiating_groups;
    
    return cp;
}

void init_seqnum(void)

// re-initialize the chunk sequence numbers for another run through the chunks.

{
    n_chunkputs = 0;
}

void insert_next_chunk_put_ready (chunkput_t *cp,tick_t insert_time)

// Generate thenext Chunk Put Ready event at insert_time, but:
//      Cannot be earlier than computed 'pace_time' per gateway.
//      Cannot be earlier than next tick (now+1)

// We run a per gateway leaky_bucket to pace transmissions.
// Credits are granted at the tick_rate
// Credits are charged at 'derived.gateway_xmit_charge'
//
// A new chunk put is delayed until the post put credits will be positive.
//
// Maximum credit accumulation is 2x chunks

{
    chunk_put_ready_t cpr;
    signed long credit;
    tick_t time = insert_time;
    gateway_t *gw;
    
    assert(cp);
    assert(cp->gateway);
    gw = cp->gateway;
 
    if (time <= now) time = now + 1;
    
    credit = time - gw->last_credited;
    gw->xmit_credit += credit;
    gw->last_credited = time;
    gw->xmit_credit -= derived.gateway_xmit_charge;
    if (gw->xmit_credit > (signed long)(2*derived.chunk_disk_write_duration))
        gw->xmit_credit = (signed long)(2*derived.chunk_disk_write_duration);
    else if (gw->xmit_credit < 0) {
        credit = -gw->xmit_credit;
        
        time = time + credit;
        ++track.n_pace_delays;
        track.aggregate_pace_delay += credit;
    }

    cpr.event.create_time = now;
    cpr.event.tllist.time = time;
    cpr.event.type = CHUNK_PUT_READY;
    cpr.cp = (chunk_put_handle_t)cp;
    cp->started = time;
        
    insert_event(cpr);
}

// add time for delay of unsolicited packet size

#define MINIMUM_UDPV6_BYTES 66
#define CHUNK_PUT_REQUEST_BYTES (MINIMUM_UDPV6_BYTES+200)

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
    
    dec_target_total_queue(rpa->target_num);
    
    if (!--cp->replicas_unacked) {
        cpa.event.create_time = e->tllist.time;
        cpa.event.tllist.time = e->tllist.time + 1;
        cpa.event.type = CHUNK_PUT_ACK;
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
    
    assert(e);

    cp = (chunkput_t *)cpa->cp;
    cp->done = e->tllist.time;
    assert(cp->seqnum);
    assert(cp->sig == 0xABCD);
    duration = cp->done - cp->started;

    assert(cp->write_qdepth >= 0);

    if (!config.terse) {
        fprintf(log_f,
                "0x%lx,%s Completion,%d,duration msec,%04.3f write_qdepth %d\n",
                e->tllist.time,protocol->tag,cp->seqnum,
                ((float)duration)/(TICKS_PER_SECOND/1000),cp->write_qdepth);
    }
    if (duration < track.min_duration) track.min_duration = duration;
    if (duration > track.max_duration) track.max_duration = duration;
    track.total_duration += duration;
    if (track.n_completions < track.max_tracked)
        track.durations[track.n_completions] = duration;
    ++track.n_completions;
    n_pending = track.n_initiated - track.n_completions;
    if (!config.terse) {
        fprintf(log_f,"%s,chunk_ack:n_completions,%lu,n_initiated,%lu",
                protocol->tag,track.n_completions,track.n_initiated);
        fprintf(log_f,",pending,%lu,per target %04.3f\n",
            n_pending,((float)n_pending)/derived.n_targets);
    }
    if (cp->write_qdepth > MAX_WRITE_QDEPTH)
        cp->write_qdepth = MAX_WRITE_QDEPTH;

    assert(!cp->replicas_unacked);
    memset(cp,0xFE,sizeof *cp);
    free(cp);
    assert(!track.mbz);
}

static int tick_compare (const void *a,const void *b)
//
// Compare two bids for qsort() by start
//
{
    const tick_t *ba = a;
    const tick_t *bb = b;
    
    return *ba < *bb ? -1
    : *ba == *bb ? 0
    : 1;
}

void report_duration_stats (void)

// report and clear the accumulated duratin and qdepth stats

{
    float avg_ticks,min_x,max_x,mbs,msecs,chunks_per_t;
    unsigned long total_write;
    unsigned n_tracked;
    const float ticks_per_ms = TICKS_PER_SECOND/(float)1000;

    printf("\nPerformance results:");
    if (track.n_completions) {
        msecs = ((double_t)now)/ticks_per_ms;
        printf("\nTotal unique chunks: initiated %lu completed %lu ",
               track.n_initiated,track.n_completions);
        printf("execution-time %.1f ms\n",msecs);
        
        avg_ticks = (float)track.total_duration/(float)track.n_completions;
        min_x = (float)track.min_duration/avg_ticks;
        max_x = ((float)track.max_duration)/avg_ticks;
        printf("# Pacing Delays: %d, %3.2f msecs\n",track.n_pace_delays,
               track.aggregate_pace_delay/ticks_per_ms);
        printf("Chunk write latency (ms): min %3.2f (%.2f * avg) average %.3f ",
               ((float)track.min_duration)/ticks_per_ms,min_x,
               avg_ticks/ticks_per_ms);
        printf("max %3.2f (%.2f * avg)\n",
               ((float)track.max_duration)/ticks_per_ms,max_x);
        n_tracked = (unsigned)track.n_completions;
        if (n_tracked > track.max_tracked) n_tracked = track.max_tracked;
        
        qsort(track.durations,n_tracked,sizeof(tick_t),tick_compare);
        
        printf("median %3.2f 90%% %3.2f 99%% %3.2f\n",
               track.durations[n_tracked*50/100]/ticks_per_ms,
               track.durations[n_tracked*90/100]/ticks_per_ms,
               track.durations[n_tracked*99/100]/ticks_per_ms);
        
        total_write = (unsigned long)track.n_completions * config.chunk_size *
            config.n_replicas;
        mbs = ((float)total_write)/(1000*1000) / derived.n_targets;
        chunks_per_t =
            (float)track.n_completions *
            ((float)config.n_replicas)/derived.n_targets;
        printf("\nAverage written per target: %6.2fMBs",mbs);
        printf(" or %4.1f chunk-replicas\n",chunks_per_t);
        printf("Average target throughput: %6.2f MB/s ",mbs*1000/msecs);

        mbs = ((float)total_write)/(1024*1024) / config.n_gateways;
        chunks_per_t = (float)track.n_completions *
                        ((float)config.n_replicas)/config.n_gateways;
        printf("Average gateway sent:%6.2fMB or %4.1f chunk-replicas\n",mbs,
               chunks_per_t);
        printf("Average gateway throughput %6.2fMB/s\n",mbs*1000/msecs);
    
    }
}

unsigned chunk_seq (chunk_put_handle_t cph)
{
    const chunkput_t *cp = (const chunkput_t *)cph;
    
    assert(cp->sig == 0xABCD);
    assert(cp->seqnum);
    return cp->seqnum;
}

gateway_t *chunk_gateway (chunk_put_handle_t cph)
{
    const chunkput_t *cp = (const chunkput_t *)cph;
    
    assert(cp->sig == 0xABCD);
    assert(cp->seqnum);
    return cp->gateway;
}
