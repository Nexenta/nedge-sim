//
//  omhudp.c
//  StorageClusterSim
//
//  Created by cait on 8/21/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//




#include "storage_cluster_sim.h"

typedef struct chunkput_omhudp {
    chunkput_t  cp;
    unsigned acked;
} chunkput_omhudp_t;

typedef enum omhudp_event_type { // extends enum event_type
    OMHUDP_CHUNK_PUT_READY = TRANSPORT_EVENT_BASE,
    OMHUDP_XMIT_RECEIVED,
    OMHUDP_RECEPTION_COMPLETE,
    OMHUDP_RECEPTION_ACK
} omhudp_event_type_t;

typedef struct ongoing_reception {
    // tracks an ongoing TCP reception to a target
    tllist_t    tllist; // time is time tcp reception started
    tick_t  credit;    // how many bits have been simulated
    tick_t  credited_thru;  // thru when?
    chunk_put_handle_t cp;  // for which chunk?
    unsigned max_ongoing_rx;    // maximum n_ongoing_receptions for target
    // over lifespan of this reception.
} ongoing_reception_t;

typedef struct omhtcp_target_t {
    // Track a omniscient hash target
    target_t    common; // common tracking fields
    unsigned n_ongoing_receptions;  // # of ongoing TCP receptions
    ongoing_reception_t orhead;     // tllist head of ongoing TCP receptions
    tick_t last_disk_write_completion;  // last disk write completion for
    // this target
    unsigned chunks_put;
    unsigned mbz;   // debugging paranoia
} omhtcp_target_t;
//
// A struct target represents the target specific data that each individual
// target would have stored separately. This includes the queue of inbound
// reservations and when the last disk write completion would have occurred.
//

static omhtcp_target_t *omhudp_tgt = NULL;


static void omniscient_target_select(chunkput_t *cp)
{
    // TODO
}

#define MINIMUM_TCPV6_BYTES 74
#define TCP_CHUNK_SETUP_BYTES (3*MINIMUM_TCPV6_BYTES+200)
// 3 packets for TCP connectino setup plus minimal pre-transfer data
// The cluster_trip_time must still be added to this.

static void handle_omhudp_chunk_put_ready (const event_t *e)
{
    const chunk_put_ready_t *cpr = (const chunk_put_ready_t *)e;
    chunkput_t *cp = (chunkput_t *)cpr->cp;
    
    assert (cp);
    assert(!cp->mbz);
    
    omniscient_target_select(cp);
    // TODO single mcast xmit
}

static void log_omhudp_chunk_put_ready (FILE *f,const event_t *e)
{
    fprintf(f,"omhtcp,CHUNK_PUT_READY\n");
}

static void handle_omhudp_reception_ack (const event_t *e)

// Handle the TCP reception ack, which occurs before the REPLICA_ACK
// The REPLICA_ACK occurs after the chunk replica is written.
// The tcp ack is sent once the chunk has been received.

{
    const tcp_reception_ack_t *tra = (const tcp_reception_ack_t *)e;
    chunkput_omhudp_t *cp = (chunkput_omhudp_t *)tra->cp;
    chunkput_t *new_cp;
    tick_t next_tcp_time;
    
    next_tcp_time = e->tllist.time;
    next_tcp_time -= 3*config.cluster_trip_time;
    if (next_tcp_time <= now) next_tcp_time = now+1;
    if (++cp->acked < config.n_replicas)
        ;
    else if ((new_cp = next_cp(cp->cp.gateway)) != NULL)
        insert_next_chunk_put_ready(new_cp,next_tcp_time);
}

static void log_omhudp_reception_ack (FILE *f,const event_t *e)
{
    const tcp_reception_ack_t *tra = (const tcp_reception_ack_t *)e;
    
    assert(e);
    fprintf(f,"0x%lx,0x%lx,non TCP_RECEPTION_ACK,0x%lx,%d,tgt,%d\n",
            e->tllist.time,e->create_time,tra->cp,chunk_seq(tra->cp),
            tra->target_num);
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
    
    omhudp_tgt = (omhtcp_target_t *)calloc(n_targets,sizeof(omhtcp_target_t));
    
    assert(omhudp_tgt);
    for (n = 0;n != n_targets;++n) {
        omhudp_tgt[n].orhead.tllist.next =
        omhudp_tgt[n].orhead.tllist.prev =
        &omhudp_tgt[n].orhead.tllist;
    }
}

void release_omhudp_targets (void)
{
    assert(omhudp_tgt);
    
    free(omhudp_tgt);
    omhudp_tgt = (omhtcp_target_t *)0;
}

static void credit_ongoing_receptions (omhtcp_target_t *t,
                                       unsigned current_num_receptions)
{
    tllist_t *pnd;
    ongoing_reception_t *ort;
    tick_t  elapsed_time,credit;
    
    assert (t);
    assert (t == &omhudp_tgt[t-omhudp_tgt]);
    
    for (pnd = t->orhead.tllist.next;
         pnd != &t->orhead.tllist;
         pnd = pnd->next)
    {
        ort = (ongoing_reception_t *)pnd;
        
        assert(t->n_ongoing_receptions > 0);
        
        if (now < ort->credited_thru)
            elapsed_time = 0L;
        else {
            elapsed_time = now - ort->credited_thru;
            ort->credited_thru = now;
        }
        credit = elapsed_time / t->n_ongoing_receptions;
        if (!credit) credit = 1;
        ort->credit += credit;
        if (current_num_receptions > ort->max_ongoing_rx)
            ort->max_ongoing_rx = current_num_receptions;
    }
}

static void schedule_tcp_reception_complete (unsigned target_num,
                                             chunk_put_handle_t cp
                                             )
{
    omhtcp_target_t *t = omhudp_tgt + target_num;
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
    trc.event.type = (event_type_t)OMHUDP_RECEPTION_COMPLETE;
    trc.cp = cp;
    assert (target_num < derived.n_targets);
    trc.target_num = target_num;
    insert_event(trc);
}

void handle_omhudp_xmit_received (const event_t *e)

// A new tcp chunk transfer to target_num is beginning 'now'
// We will simulate a miraculous TCP congestion algorithm which *instantly*
// adjust all N flows to be even rate. This is an ideal that can never be
// achieved, but is clearly the goal of all TCP congestion control algorihtms.
//
// The steps
//      Credit existing flows through the present.
//      create the new flow
//      if this was the first flow then schedule the tcp_reception_complete
//      event, otherwise just allow the existing event to complete. This
//      will be slightly early (because there are now n+1 flows instead of
//      n flows, so when that event triggers only (n-1)/n of the payload
//      would have been transferred. We'll just update the credits and
//      re-issue the new tcp_reception_complete event

{
    const tcp_xmit_received_t *txr = (const tcp_xmit_received_t *)e;
    ongoing_reception_t *ort = calloc(1,sizeof *ort);
    tllist_t *tp;
    ongoing_reception_t *p;
    tllist_t *insert_point;
    
    omhtcp_target_t *t;
    
    assert (e);
    
    t = omhudp_tgt + txr->target_num;
    ++t->chunks_put;
    
    fprintf(log_f,"@0x%lx Ongoing Reception,ox%p,target,%d,CP.0x%lx,%d",
            e->tllist.time,ort,txr->target_num,txr->cp,chunk_seq(txr->cp));
    if (t->n_ongoing_receptions) {
        credit_ongoing_receptions(t,t->n_ongoing_receptions+1);
        fprintf(log_f,",Prior CPs");
        for (tp = t->orhead.tllist.next; tp != &t->orhead.tllist; tp = tp->next) {
            p = (ongoing_reception_t *)tp;
            fprintf(log_f,",0x%lx,%d",p->cp,chunk_seq(p->cp));
        }
    }
    fprintf(log_f,"\n");
    assert(ort);
    ort->tllist.time = e->tllist.time;
    ort->credit = 0;
    ort->credited_thru = e->tllist.time;
    ort->cp = txr->cp;
    ort->max_ongoing_rx = t->n_ongoing_receptions + 1;
    
    insert_point = (tllist_t *)tllist_find(&t->orhead.tllist,ort->tllist.time);
    tllist_insert(insert_point,&ort->tllist);
    
    if (++t->n_ongoing_receptions == 1)
        schedule_tcp_reception_complete (txr->target_num,txr->cp);
}

void log_omhudp_xmit_received (FILE *f,const event_t *e)
{
    const tcp_xmit_received_t *txr = (const tcp_xmit_received_t *)e;
    
    if (!config.terse) {
        assert(e);
        fprintf(f,"0x%lx,0x%lx,non TCP_XMIT_RECEIVED,0x%lx,%d,tgt,%d\n",
                e->tllist.time,e->create_time,txr->cp,chunk_seq(txr->cp),
                txr->target_num);
    }
}

void handle_omhudp_reception_complete (const event_t *e)

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
    tcp_reception_ack_t tcp_ack;
    ongoing_reception_t *ort,*ort_next;
    disk_write_start_t dws;
    omhtcp_target_t *t;
    tick_t write_start,write_completion;
    unsigned n;
    tick_t write_variance =
    derived.chunk_disk_write_duration/config.write_variance;
    tick_t write_duration = derived.chunk_disk_write_duration
    - write_variance/2
    +  (rand() % write_variance);
    
    assert (e); (void)e;
    
    t = omhudp_tgt + trc->target_num;
    tcp_ack.target_num = trc->target_num;
    dws.target_num = trc->target_num;
    
    assert(t->n_ongoing_receptions);
    credit_ongoing_receptions(t,t->n_ongoing_receptions);
    
    for (ort = (ongoing_reception_t *)t->orhead.tllist.next,n=0;
         &ort->tllist != &t->orhead.tllist;
         ort = ort_next,++n)
    {
        if (ort->credit < derived.chunk_tcp_xmit_duration) {
            schedule_tcp_reception_complete (trc->target_num,ort->cp);
            break;
        }
        tcp_ack.event.create_time = e->tllist.time;
        tcp_ack.event.tllist.time = e->tllist.time + config.cluster_trip_time;
        tcp_ack.event.type = (event_type_t)OMHUDP_RECEPTION_ACK;
        tcp_ack.cp = ort->cp;
        tcp_ack.max_ongoing_rx = ort->max_ongoing_rx;
        insert_event(tcp_ack);
        
        dws.event.create_time = e->tllist.time;
        write_start = (t->last_disk_write_completion > e->tllist.time)
        ? t->last_disk_write_completion
        : e->tllist.time;
        write_completion = write_start + write_duration;
        
        dws.event.create_time = e->tllist.time;
        dws.event.tllist.time = write_start;
        t->last_disk_write_completion = write_completion;
        dws.expected_done = write_completion;
        dws.event.type = DISK_WRITE_START;
        dws.cp = ort->cp;
        assert(chunk_seq(ort->cp));
        
        insert_event(dws);
        
        ort_next = (ongoing_reception_t *)ort->tllist.next;
        assert(ort_next);
        assert(&ort_next->tllist == &t->orhead.tllist || ort_next->cp != ort->cp);
        tllist_remove(&ort->tllist);
        memset(ort,0xFD,sizeof *ort);
        free(ort);
        --t->n_ongoing_receptions;
    }
}

void log_omhudp_reception_complete (FILE *f,const event_t *e)
{
    const tcp_reception_complete_t *txr = (const tcp_reception_complete_t *)e;
    
    if (!config.terse) {
        assert(e);
        fprintf(f,"0x%lx,0x%lx,non TCP_XMIT_RECEIVED,0x%lx,%d,tgt,%d\n",
                e->tllist.time,e->create_time,txr->cp,chunk_seq(txr->cp),
                txr->target_num);
    }
}

#define MAX_TALLY 2048
void report_omhudp_chunk_distribution (FILE *f)

// Report distribution of chunks to targets to log_f

{
    unsigned tally[MAX_TALLY];
    const omhtcp_target_t *tp;
    const omhtcp_target_t *tp_lim;
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
    .init_target = init_omhudp_targets,
    .target = omhudp_target,
    .report_chunk_distribution = report_omhudp_chunk_distribution,
    .release_targets = release_omhudp_targets,
    .h = {
        {handle_omhudp_chunk_put_ready,log_omhudp_chunk_put_ready},
        {handle_omhudp_xmit_received,log_omhudp_xmit_received},
        {handle_omhudp_reception_complete,log_omhudp_reception_complete},
        {handle_omhudp_reception_ack,log_omhudp_reception_ack}
    }
};







