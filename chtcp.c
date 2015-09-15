//
//  chtcp.c
//  StorageClusterSim
//
//  Created by Caitlin Bestler on 8/17/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#include "storage_cluster_sim.h"

typedef struct chunkput_chucast {
    chunkput_t  cp;
    unsigned ch_targets[MAX_REPLICAS]; // selected targets
    unsigned repnum;                    // # of replicas previously generated.
    unsigned acked;
    unsigned max_ongoing_rx;    // maximum n_ongoing_receptions for any target
} chunkput_chucast_t;

typedef enum chctp_event_type { // extends enum event_type
    CHUCAST_CHUNK_PUT_READY = TRANSPORT_EVENT_BASE,
    CHUCAST_XMIT_RECEIVED,  // initiation of a new flow from a gateway to target
    CHUCAST_XMIT_SLOW,  // a new flow has been enabled, slow all flows after RTT
    CHUCAST_XMIT_SPEED, // a flow completed a RTT earlier
                        // increase the rate for the remaining flows.
    CHUCAST_RECEPTION_COMPLETE, // earlies estimate flow completion to target
    CHUCAST_RECEPTION_ACK   // transport layer ack that chunk was sent
                            // this proceeds the REPLICA_ACK which is not
                            // sent until the Chunk has been written to disk.
} chctp_event_type_t;

typedef struct ongoing_reception {
    // tracks an ongoing TCP reception to a target
    tllist_t    tllist; // time is time tcp reception started
    tick_t      credit;    // how many bits have been simulated
    tick_t      credited_thru;  // thru when?
    chunk_put_handle_t cp;  // for which chunk?
    unsigned max_ongoing_rx;    // maximum n_ongoing_receptions for target
    // over lifespan of this reception.
} ongoing_reception_t;

typedef struct chucast_xmit_slow {
    event_t event;          // chucast_xmit_slow is an event
    unsigned target_num;    // Target with new flow
    ongoing_reception_t *new_flow;
    chunk_put_handle_t  cp; // Handle of the chunk put for new_flow
} chucast_xmit_slow_t;

typedef struct chucast_xmit_speed {
     event_t event;          // chucast_xmit_slow is an event
    unsigned target_num;    // Target with modified flow set
} chucast_xmit_speed_t;

typedef struct chucast_target {
    // Track a chucast target
    target_t    common; // common tracking fields
    unsigned n_ongoing_receptions;  // # of ongoing TCP receptions
    ongoing_reception_t orhead;     // tllist head of ongoing TCP receptions
    tick_t last_disk_write_completion;  // last disk write completion for
                                        // this target
    unsigned chunks_put;
    unsigned mbz;   // debugging paranoia
} chucast_target_t;
//
// A struct target represents the target specific data that each individual
// target would have stored separately. This includes the queue of inbound
// reservations and when the last disk write completion would have occurred.
//

static chucast_target_t *chucast_tgt = NULL;

static void select_chucast_targets (chunkput_chucast_t *c)

// select config.n_replicas different targets
// store them in ->ch_targets[0..n_repicas-1]
//
// for each pick select at random
//      if previously selected try next target

{
    unsigned n,t,i;
    
    assert (config.n_replicas < derived.n_targets);
    
    fprintf(log_f,"@0x%lx,CP,0x%p,%d,targets selected",now,c,c->cp.seqnum);
    for (n = 0; n != config.n_replicas;++n) {
        t = rand() % derived.n_targets;
        for (i = 0; i < n; ++i) {
            while (c->ch_targets[i] == t) {
                t = (t + 1) % derived.n_targets;
                i = 0;
            }
        }
        c->ch_targets[n] = t;
        fprintf(log_f,",%d",t);
    }
    fprintf(log_f,"\n");
}

#define MINIMUM_TCPV6_BYTES 74
#define TCP_CHUNK_SETUP_BYTES (3*MINIMUM_TCPV6_BYTES+200)
// 3 packets for TCP connectino setup plus minimal pre-transfer data
// The cluster_trip_time must still be added to this.

target_t *chucast_target (unsigned target_num)
{
    return &chucast_tgt[target_num].common;
}

static void next_tcp_replica_xmit (chunkput_chucast_t *cp,tick_t time_now)

// Schedule the next TCP transmit start after the previous tcp transmit for
// the same object has completed

{
    tcp_xmit_received_t txr;
    unsigned r;
    
    if (cp->cp.replicas_unacked) {
        txr.event.create_time = time_now;
        txr.event.tllist.time = time_now + config.cluster_trip_time*3 +
        TCP_CHUNK_SETUP_BYTES*8;
        txr.event.type = (event_type_t)CHUCAST_XMIT_RECEIVED;
        txr.cp = (chunk_put_handle_t)cp;
        r = cp->repnum++;
        txr.target_num = cp->ch_targets[r];
        inc_target_total_queue(txr.target_num);
        insert_event(txr);
    }
}

static void handle_chucast_chunk_put_ready (const event_t *e)
{
    const chunk_put_ready_t *cpr = (const chunk_put_ready_t *)e;
    chunkput_chucast_t *cp = (chunkput_chucast_t *)cpr->cp;
    
    assert (cp);
    assert(!cp->cp.mbz);
    
    select_chucast_targets(cp);
    next_tcp_replica_xmit(cp,e->tllist.time);
}

static void log_chucast_chunk_put_ready (FILE *f,const event_t *e)
{
    fprintf(f,"chucast,CHUNK_PUT_READY\n");
}

static void remove_tcp_reception_target (chunkput_chucast_t *cp,
                                         unsigned target_num)

// this is a sanity checking diagnostic.
// It does notcontribute to the final results.

{
    unsigned  *p;
    
    for (p = cp->ch_targets;
         p != cp->ch_targets+config.n_replicas;
         ++p)
    {
        if (*p == target_num) {
            *p = 0;
            return;
        }
    }
}

static void handle_chucast_reception_ack (const event_t *e)

// Handle the TCP reception ack, which occurs before the REPLICA_ACK
// The REPLICA_ACK occurs after the chunk replica is written.
// The tcp ack is sent once the chunk has been received.

{
    const tcp_reception_ack_t *tra = (const tcp_reception_ack_t *)e;
    chunkput_chucast_t *cp = (chunkput_chucast_t *)tra->cp;
    chunkput_t *new_cp;
    tick_t next_tcp_time;
    
    remove_tcp_reception_target(cp,tra->target_num);
    
    if (tra->max_ongoing_rx > cp->max_ongoing_rx)
        cp->max_ongoing_rx  = tra->max_ongoing_rx;
    next_tcp_time = e->tllist.time;
    next_tcp_time -= 3*config.cluster_trip_time;
    if (next_tcp_time <= now) next_tcp_time = now+1;
    if (++cp->acked < config.n_replicas)
        next_tcp_replica_xmit(cp,next_tcp_time);
    else if ((new_cp = next_cp(cp->cp.gateway,
                               sizeof(chunkput_chucast_t))) != NULL)
        insert_next_chunk_put_ready(new_cp,next_tcp_time);
}

static void log_chucast_reception_ack (FILE *f,const event_t *e)
{
    const tcp_reception_ack_t *tra = (const tcp_reception_ack_t *)e;
    
    assert(e);
    fprintf(f,"0x%lx,0x%lx,chucast,TCP_RECEPTION_ACK,0x%lx,%d,tgt,%d\n",
            e->tllist.time,e->create_time,tra->cp,chunk_seq(tra->cp),
            tra->target_num);
}

void init_chucast_targets(unsigned n_targets)
//
// Initialize the target subsystem, specifically the irhead for each
// entry in the target_t array 't' must be set to empty.
// Setting the ir_head to point to itself makes an empty list.
//
{
    unsigned n;
    
    assert(!chucast_tgt);
    
    chucast_tgt =
        (chucast_target_t *)calloc(n_targets,sizeof(chucast_target_t));
    
    assert(chucast_tgt);
    for (n = 0;n != n_targets;++n) {
        chucast_tgt[n].orhead.tllist.next = chucast_tgt[n].orhead.tllist.prev =
        &chucast_tgt[n].orhead.tllist;
    }
}

void release_chucast_targets (void)
{
    assert(chucast_tgt);
    
    free(chucast_tgt);
    chucast_tgt = (chucast_target_t *)0;
}

static void complete_ongoing_reception (chucast_target_t *t,
                                        ongoing_reception_t *ort)
{
    tcp_reception_ack_t tcp_ack;
    disk_write_start_t dws;
    tick_t write_start;
    tick_t write_completion;
    chucast_xmit_speed_t cxsp;
    tick_t write_variance =
        derived.chunk_disk_write_duration/config.write_variance;
    tick_t write_duration = derived.chunk_disk_write_duration
        - write_variance/2
        +  (rand() % write_variance);
    unsigned tgt_num = (unsigned)(t - chucast_tgt);

    tcp_ack.event.create_time = now;
    tcp_ack.event.tllist.time = now + config.cluster_trip_time;
    tcp_ack.event.type = (event_type_t)CHUCAST_RECEPTION_ACK;
    tcp_ack.cp = ort->cp;
    tcp_ack.target_num = tgt_num;
    tcp_ack.max_ongoing_rx = ort->max_ongoing_rx;
    insert_event(tcp_ack);
    
    dws.event.create_time = now;
    write_start = (t->last_disk_write_completion > now)
        ? t->last_disk_write_completion
        : now;
    write_completion = write_start + write_duration;
    
    dws.event.create_time = now;
    dws.event.tllist.time = write_start;
    t->last_disk_write_completion = write_completion;
    dws.expected_done = write_completion;
    dws.event.type = DISK_WRITE_START;
    dws.cp = ort->cp;
    dws.target_num = tgt_num;
    
    insert_event(dws);
    
    tllist_remove(&ort->tllist);
    memset(ort,0xFD,sizeof *ort);
    free(ort);
    
    cxsp.event.type = (event_type_t)CHUCAST_XMIT_SPEED;
    cxsp.target_num = (unsigned)(t - chucast_tgt);
    cxsp.event.tllist.time = now + 2*config.cluster_trip_time;
    cxsp.event.create_time = now;
    insert_event(cxsp); // causes eventual -- t->n_ongoing_receptions
}

static void credit_ongoing_receptions (chucast_target_t *t,
                                       unsigned current_num_receptions)
{
    tllist_t *pnd,*pnd_next;
    ongoing_reception_t *ort;
    tick_t  elapsed_time,credit;
    unsigned n;
    
    assert (t);
    assert (t == &chucast_tgt[t-chucast_tgt]);
    
    if (current_num_receptions) {
        for (pnd = t->orhead.tllist.next,n=1;
             pnd != &t->orhead.tllist;
             pnd = pnd_next,++n)
        {
            ort = (ongoing_reception_t *)pnd;
            pnd_next = pnd->next;
            
            assert(now >= ort->credited_thru);
            elapsed_time = now - ort->credited_thru;
            ort->credited_thru = now;
            
            credit = elapsed_time / current_num_receptions;
            if (!credit) credit = 1;
            ort->credit += credit;
            fprintf(log_f,"Credit,%lu,total,%lu,of,%lu,thru,%lu,target,%ld,ort,%p",
                    credit,ort->credit,derived.chunk_tcp_xmit_duration,
                    ort->credited_thru,t-chucast_tgt,ort);
            fprintf(log_f,",n,%d,ongoing,%d",n,current_num_receptions);
            fprintf(log_f,",cp,%d",chunk_seq(ort->cp));
            fprintf(log_f,",elapsed_time,%lu\n",elapsed_time);
            if (current_num_receptions > ort->max_ongoing_rx)
                ort->max_ongoing_rx = current_num_receptions;
            if (ort->credit >= derived.chunk_tcp_xmit_duration)
                complete_ongoing_reception(t,ort);
        }
    }
}

static void schedule_tcp_reception_complete (unsigned target_num,
                                             chunk_put_handle_t cp
                                             )
{
    chucast_target_t *t = chucast_tgt + target_num;
    tcp_reception_complete_t trc;
    ongoing_reception_t *ort;
    tick_t remaining_xfer;
    
    assert(t);
    
    ort = (ongoing_reception_t *)t->orhead.tllist.next;
    assert(ort  &&  &ort->tllist != &t->orhead.tllist);
    
    trc.event.create_time = now;
    assert(derived.chunk_tcp_xmit_duration > ort->credit);
    remaining_xfer = derived.chunk_tcp_xmit_duration - ort->credit;
    assert(t->n_ongoing_receptions);
    trc.event.tllist.time = now + remaining_xfer*t->n_ongoing_receptions;
    trc.event.type = (event_type_t)CHUCAST_RECEPTION_COMPLETE;
    trc.cp = cp;
    assert (target_num < derived.n_targets);
    trc.target_num = target_num;
    insert_event(trc);
}

static void handle_chucast_xmit_received (const event_t *e)

// A new tcp chunk transfer to target_num is beginning 'now'
// We will simulate a miraculous TCP congestion algorithm which *instantly*
// adjust all N flows to be even rate. This is an ideal that can never be
// achieved, but is clearly the goal of all TCP congestion control algorihtms.
//
// The steps
//      Credit existing flows through the present.
//      create the new flow
//      create chucast_xmit_slow event after RTT to slow all existing
//          connections to make room for the new one.

{
    const tcp_xmit_received_t *txr = (const tcp_xmit_received_t *)e;
    ongoing_reception_t *ort = calloc(1,sizeof *ort);
    chucast_xmit_slow_t cxs;
    tllist_t *tp;
    ongoing_reception_t *p;
    chucast_target_t *t;
    unsigned n;
    
    assert (e);
    
    t = chucast_tgt + txr->target_num;
    ++t->chunks_put;
    
    if (!t->n_ongoing_receptions) {
        fprintf(log_f,
                "@0x%lx,FirstOngoingReception,ox%p,target,%d,CP.0x%lx,%d\n",
                e->tllist.time,ort,txr->target_num,txr->cp,chunk_seq(txr->cp));
    }
    else {
        credit_ongoing_receptions(t,t->n_ongoing_receptions);
        fprintf(log_f,"@0x%lx,OngoingReception,ox%p,target,%d,CP.0x%lx,%d",
                e->tllist.time,ort,txr->target_num,txr->cp,chunk_seq(txr->cp));
        fprintf(log_f,",%d,Prior CPs",t->n_ongoing_receptions);
        for (tp = t->orhead.tllist.next,n=0;
             tp != &t->orhead.tllist;
             tp = tp->next,++n)
        {
            p = (ongoing_reception_t *)tp;
            fprintf(log_f,",0x%lx,%d",p->cp,chunk_seq(p->cp));
        }
        fprintf(log_f,"(%d)\n",n);
    }
    assert(ort);
    ort->tllist.time = now;
    ort->credit = 0;
    ort->credited_thru = now;
    ort->cp = txr->cp;
    ort->max_ongoing_rx = t->n_ongoing_receptions + 1;
    
    cxs.event.type = (event_type_t)CHUCAST_XMIT_SLOW;
    cxs.event.tllist.time = now + 2*config.cluster_trip_time;
    cxs.new_flow = ort;
    cxs.target_num = txr->target_num;
    cxs.cp = txr->cp;
    
    insert_event(cxs);
}

static void log_chucast_xmit_received (FILE *f,const event_t *e)
{
    const tcp_xmit_received_t *txr = (const tcp_xmit_received_t *)e;
    
    if (!config.terse) {
        assert(e);
        fprintf(f,"0x%lx,0x%lx,chucast,TCP_XMIT_RECEIVED,0x%lx,%d,tgt,%d\n",
                e->tllist.time,e->create_time,txr->cp,chunk_seq(txr->cp),
                txr->target_num);
    }
}

static void handle_chucast_xmit_slow (const event_t *e)
{
    const chucast_xmit_slow_t *cxs = (const chucast_xmit_slow_t *)e;
    ongoing_reception_t *ort = cxs->new_flow;
    chucast_target_t *t = (chucast_target_t *)chucast_target(cxs->target_num);
    tllist_t *insert_point;
 
    credit_ongoing_receptions(t,++t->n_ongoing_receptions);
    insert_point = (tllist_t *)tllist_find(&t->orhead.tllist,ort->tllist.time);
    assert(insert_point);
    tllist_insert(insert_point,&ort->tllist);

    if (t->n_ongoing_receptions == 1)
        schedule_tcp_reception_complete (cxs->target_num,cxs->cp);
}

static void log_chucast_xmit_slow (FILE *f,const event_t *e)
{
    const chucast_xmit_slow_t *cxs = (const chucast_xmit_slow_t *)e;
    chucast_target_t *t = (chucast_target_t *)chucast_target(cxs->target_num);
    
    if (!config.terse)
        fprintf(f,"CHUCAST_XMIT_SLOW,cp,%lu,tgt,%d,#ongoing,%d\n",cxs->cp,
                cxs->target_num,t->n_ongoing_receptions);
}

static void handle_chucast_xmit_speed (const event_t *e)
{
    const chucast_xmit_speed_t *cxsp = (const chucast_xmit_speed_t *)e;
    chucast_target_t *t = (chucast_target_t *)chucast_target(cxsp->target_num);
    
    if (--t->n_ongoing_receptions)
        credit_ongoing_receptions(t,t->n_ongoing_receptions);
}

static void log_chucast_xmit_speed (FILE *f,const event_t *e)
{
    const chucast_xmit_speed_t *cxsp = (const chucast_xmit_speed_t *)e;
    chucast_target_t *t = (chucast_target_t *)chucast_target(cxsp->target_num);
    if (!config.terse)
        fprintf(f,"CHUCAST_XMIT_SPEED,tgt,%d,#ongoing,%d\n",cxsp->target_num,
                t->n_ongoing_receptions);
}

static void handle_chucast_reception_complete (const event_t *e)

// handle the expected completion of a TCP chunk reception.
//
// while there is an ongoing reception queue (orhead) for the target
//      if the lead ongoing reception does not have enough credit yet
//          schedule the next tcp_reception_complete event
//          break
//      Make a Disk Write Completion event for the completed ongoing reception
//      consume the ongoing reception

{
    const tcp_reception_complete_t *trc = (const tcp_reception_complete_t *)e;
    ongoing_reception_t *ort;
    chucast_target_t *t;
    assert (e);
    
    t = chucast_tgt + trc->target_num;
    
    credit_ongoing_receptions(t,t->n_ongoing_receptions);
    if (t->orhead.tllist.next != &t->orhead.tllist) {
        ort = (ongoing_reception_t *)t->orhead.tllist.next;
        schedule_tcp_reception_complete (trc->target_num,ort->cp);
    }
}

static void log_chucast_reception_complete (FILE *f,const event_t *e)
{
    const tcp_reception_complete_t *txr = (const tcp_reception_complete_t *)e;
    
    if (!config.terse) {
        assert(e);
        fprintf(f,"0x%lx,0x%lx,chucast,TCP_XMIT_RECEIVED,0x%lx,%d,tgt,%d\n",
                e->tllist.time,e->create_time,txr->cp,chunk_seq(txr->cp),
                txr->target_num);
    }
}

#define MAX_TALLY 2048
void report_chucast_chunk_distribution (FILE *f)

// Report distribution of chunks to targets to log_f

{
    unsigned tally[MAX_TALLY];
    const chucast_target_t *tp;
    const chucast_target_t *tp_lim;
    unsigned n,max_n;
    
    memset(tally,0,sizeof tally);
    for (tp =  chucast_tgt, tp_lim = chucast_tgt + derived.n_targets, max_n = 0;
         tp != tp_lim;
         ++tp)
    {
        n = tp->chunks_put;
        if (n >= MAX_TALLY) n = MAX_TALLY-1;
        ++tally[n];
        if (n > max_n) max_n = n;
    }
    fprintf(f,"CHUCAST Chunks per target distribution:\n");
    for (n = 0;;++n) {
        fprintf(f,"%d --> %d\n",n,tally[n]);
        if (n == max_n) break;
    }
}

protocol_t chucast_prot = {
    .tag = "chucast",
    .name = "Consistent Hash-Unicast",
    .cp_size = sizeof(chunkput_chucast_t),
    .do_me = false,
    .init_target = init_chucast_targets,
    .target = chucast_target,
    .report_chunk_distribution = report_chucast_chunk_distribution,
    .release_targets = release_chucast_targets,
    .h = {
        {handle_chucast_chunk_put_ready,log_chucast_chunk_put_ready},
        {handle_chucast_xmit_received,log_chucast_xmit_received},
        {handle_chucast_xmit_slow,log_chucast_xmit_slow},
        {handle_chucast_xmit_speed,log_chucast_xmit_speed},
        {handle_chucast_reception_complete,log_chucast_reception_complete},
        {handle_chucast_reception_ack,log_chucast_reception_ack}
    }
};


