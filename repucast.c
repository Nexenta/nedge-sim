//
//  repucast.c
//  StorageClusterSim
//
//  Created by Caitlin Bestler on 8/17/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//
// Variation of replicast that uses unicast delivery.



#include "storage_cluster_sim.h"

typedef struct chunkput_repu {
    chunkput_t  cp;
    bid_t       bids[MAX_TARGETS_PER_NG];
                // collected bid response
    unsigned nbids; // # of bids collected
    unsigned ch_targets[MAX_REPLICAS]; // selected targets
    unsigned repnum;                    // # of replicas previously generated
    unsigned acked;
    unsigned responses_uncollected; // Chunk Put responses still to be collected
    unsigned max_ongoing_rx;
    unsigned replicas_unacked; // TODO: initialize this
} chunkput_repu_t;

typedef enum repu_event_type  { // exends enum event_type
    REPU_CHUNK_PUT_READY = TRANSPORT_EVENT_BASE,
    REPU_CHUNK_PUT_REQUEST_RECEIVED,
    REPU_CHUNK_PUT_RESPONSE_RECEIVED,
    REPU_XMIT_ELIGIBLE,
    REPU_XMIT_START,
    REPU_XMIT_COMPLETE,
    REPU_XMIT_ACK
} repu_event_type_t;

// Event Flow for Replicast - Unicast
//
//   Gateway---PUT_REQUEST--->StorageNode(each in NG)
//   Gateway<---PUT_RESPONSE--StorageNode(each in NG)
//   Gateway----ACCEPT------->StorageNode(each in NG)
//   Gateway----UnicastTransfer--->StorageNode(1st accepted)
//          Storage Server disk write actions
//   Gateway<---XmitAck---------------StorageNode(1st accepted)
//      Gateway is not able to transmit the next repica
//   Gateway----UnicastTransfer--->StorageNode(next accepted)
//          Storage Server disk write actions
//   ...
//   Gateway<---ReplicaAck------------StorageNode(1st accepted)
//      This is acknowledgement that the Chunk has been stored.
//   ...
//   Gateway<-- ChunkAck (inferred)

typedef struct ongoing_reception {
    // tracks an ongoing TCP reception to a target
    tllist_t    tllist;     // time is time tcp reception started
    tick_t      credit;    // how many bits have been simulated
    tick_t      credited_thru;  // thru when?
    chunk_put_handle_t cp;      // for which chunk?
    unsigned    max_ongoing_rx;     // maximum n_ongoing_receptions for target
                                    // over lifespan of this reception.
} ongoing_reception_t;

typedef struct repu_target_t {       // Track replicast target
    target_t    common;             // common fields
    unsigned    n_ongoing_receptions;  // # of ongoing TCP receptions
    ongoing_reception_t orhead;
    unsigned    write_queue_depth;     // depth of write queue for this target
    tick_t      last_disk_write_completion;  // last disk write completion for
                                            // this target
    unsigned    chunks_put;       // total number of chunks put to this target
    unsigned    mbz;   // debugging paranoia
} repu_target_t;
//
// A struct target represents the target specific data that each individual
// target would have stored separately. This includes the queue of inbound
// reservations and when the last disk write completion would have occurred.
//
typedef struct repu_rendezvous_xfer_received {
    event_t event;          // rep_rendezvous_transfer_receieved is an event
    chunk_put_handle_t  cp; // Handle of the chunk put
    unsigned target_num;    // The target that received this rendezvous transfer
} repu_rendezvous_xfer_received_t;
//
// The Rendezvous Transfer is received by each selected target for a specific
// chunk put. it occurs when the full chunk transfer would be compelte.
//
// It is scheduled at the same time as the chunk_put_accept for the subset
// of the negotiating group which was accepted.
//

static repu_target_t *repu = NULL;

//
// This event represents receipt ofthe first byte  of a TCP trasmission of chunk
// 'cp' to 'target_num'.
// It is generated by the gateway and consumed by the target.
//
// The target will then simulate reception of this and any other concurrently
// received TCP flows to this target to schedule REPU_XMIT_COMPLETE events
//
// This is a very generous model of TCP congestions, where we allow some future
// TCP congestion algorithm to *instantly* converge on evenly dividing the
// egress port capacity evenly over N payload flows.
//

typedef struct repu_xmit_start {
    event_t     event;          // repu_xmit_complete is an event
    chunk_put_handle_t  cp;     // Handle of the chunk put
    unsigned    target_num;     // Target where this reception completed
} repu_xmit_start_t;

//
// This event models signals the schedule completion of the earliest
// ongoing reception for 'target_num'. Note that if another tcp reception
// was added in the interim that reception might not be actually complete.
// In that case the amount actually transferred is calculated and a new
// event is scheduled.
//

typedef struct repu_xmit_ack {
    event_t event;          // repu_xmit_ack is an event
    chunk_put_handle_t  cp; // handle ofthe chunk put
    unsigned target_num;    // ack is from this target
    unsigned max_ongoing_rx;    // maximum n_ongoing_receptions for target
    // over lifespan of this transmission.
} repu_xmit_ack_t;

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

#if 0
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
#endif

static void handle_repu_xmit_eligible (const event_t *e)
//
// if cp->currently_xmitting
//      done (will xmit after xmit_ack event)
// set repu_xmit_elgible event for when first bid should start xmitting
//      (will launch repu_xfer_received event then)

{
    
}

static void log_repu_xmit_eligible (FILE *f,const event_t *e)
{
    
}

static void handle_repu_chunk_put_response_received (const event_t *e)
//
// if cp->unxmitted_replicas == 0
//      accept the source of this declining the bid
//      done
//  add bid to queue
//  if there is an excess
//      accept the source of the excess bid and declining their bid
//      remove excess from queue
//  generate_next_event_for_cp

{

}

static void log_repu_chunk_put_response_received (FILE *f,const event_t *e)
{
    const rep_chunk_put_response_received_t *cpresp =
    (rep_chunk_put_response_received_t *)e;
    
    if (!config.terse) {
        assert(e);
        fprintf(f,"0x%lx,0x%lx,REPU_CHUNK_PUT_RESPONSE_RCVD,0x%lx,%d,",
                e->tllist.time,e->create_time,cpresp->cp,chunk_seq(cpresp->cp));
        fprintf(f,"tgt,%d,bid,0x%lx,0x%lx\n",cpresp->target_num,
                cpresp->bid_start,cpresp->bid_lim);
    }
}

#define MINIMUM_TCPV6_BYTES 74
#define CHUNK_PUT_REQUEST_BYTES (MINIMUM_TCPV6_BYTES+200)

static void handle_repu_chunk_put_ready (const event_t *e)

// Repucast sends the same multicast chunk put request as used for repicast

{
    const chunk_put_ready_t *cpr = (const chunk_put_ready_t *)e;
    chunkput_repu_t *cp = (chunkput_repu_t *)cpr->cp;
    rep_chunk_put_request_received_t cprr;
    
    assert (cp);
    assert(!cp->cp.mbz);
    
    cprr.event.create_time = e->tllist.time;
    cprr.event.tllist.time   = e->tllist.time +
        config.cluster_trip_time + CHUNK_PUT_REQUEST_BYTES*8;
    cprr.event.type = (event_type_t)REPU_CHUNK_PUT_REQUEST_RECEIVED;
    cprr.cp = (chunk_put_handle_t)cp;
    cp->responses_uncollected = config.n_targets_per_ng;
    
    /* for each Target in randomly selected negotiating group.
     * the Targets are assigned round-robin to Negotiating Groups.
     *
     * Actual Negotiating Groups are selected based on cryptographic hash
     * of the payload (for payload chunks) or the object name (for metadata)
     */
    cp->cp.ng = rand() % config.n_negotiating_groups;
    for_ng(cprr.target_num,cp->cp.ng)
        insert_event(cprr);
}

static void log_repu_chunk_put_ready (FILE *f,const event_t *e)
{
    gateway_t *gateway;
    const chunk_put_ready_t *cpr = (const chunk_put_ready_t *)e;
    
    if (!config.terse) {
        assert(e);
        fprintf(f,"0x%lx,0x%lx,REPU_CHUNK_PUT_READY,0x%lx,%d",
                e->tllist.time,e->create_time,cpr->cp,chunk_seq(cpr->cp));
        gateway = chunk_gateway(cpr->cp);
        fprintf(f,",Gateway,%d,chunks,%d\n",gateway->num,gateway->n_chunks);
    }
}

static target_t *repu_target (unsigned target_num)
{
    return &repu[target_num].common;
}

static void init_repu_targets(unsigned n_targets)
//
// Initialize the target subsystem, specifically the irhead for each
// entry in the target_t array 't' must be set to empty.
// Setting the ir_head to point to itself makes an empty list.
//
{
    unsigned n;
    
    repu = (repu_target_t *)calloc(n_targets,sizeof(repu_target_t));
    assert(repu);
  
    for (n=0;n != n_targets;++n)
        repu[n].orhead.tllist.next = repu[n].orhead.tllist.prev =
        &repu[n].orhead.tllist;
}

static void release_repu_targets (void)
{
    free(repu);
    repu = (repu_target_t *)0;
}

static void make_bid (unsigned target_num,
                      chunk_put_handle_t cp,
                      tick_t *start,
                      tick_t *lim,
                      tick_t *ack_at,
                      int *qdepth)

// Make a bid to receive 'chunk_num' on 'target_num' later than 'now' lasting
// at least 'duration'. With repucast there is no actual reservation made.
// the gateway selects based on the estimated write start and the transfer
// takes as long as it takes.
//

{
    repu_target_t *tp = repu + target_num;
    tick_t s;
    tick_t estimated_write_start;
    
    assert(start);
    assert(lim);
    assert(ack_at);
    assert(target_num < derived.n_targets);
    
    // make initial guess
    *start = s = now + 2*config.cluster_trip_time +
                 config.replicast_packet_processing_penalty;
    
    estimated_write_start = *lim;
    if (tp->last_disk_write_completion > estimated_write_start)
        estimated_write_start = tp->last_disk_write_completion;
    //
    // This is an *estimated* ack, we don't know the variation yet
    //
    *ack_at = estimated_write_start + config.cluster_trip_time +
    derived.chunk_disk_write_duration;

    assert(chunk_seq(cp));
    
    // todo: how do we measure qdepth?
    assert(qdepth);
}

static void handle_repu_chunk_put_request_received (const event_t *e)

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
    cpresp.event.type = (event_type_t)REPU_CHUNK_PUT_RESPONSE_RECEIVED;
    cpresp.cp = cpr->cp;
    cpresp.target_num = cpr->target_num;
    make_bid(cpresp.target_num,cpresp.cp,&cpresp.bid_start,&cpresp.bid_lim,
             &cpresp.estimated_ack,&cpresp.qdepth);
    insert_event(cpresp);
}

static void log_repu_chunk_put_request_received (FILE *f,const event_t *e)
{
    const rep_chunk_put_request_received_t *cpreq =
    (const rep_chunk_put_request_received_t *)e;
    
    if (!config.terse) {
        assert(e);
        fprintf(f,"0x%lx,0x%lx,REPU_CHUNK_PUT_REQUEST_RECEIVED,0x%lx,%d",
                e->tllist.time,e->create_time,cpreq->cp,chunk_seq(cpreq->cp));
        fprintf(f,",tgt,%d\n",cpreq->target_num);
    }
}

static void handle_repu_xmit_start (const event_t *e)
{
    
}

static void log_repu_xmit_start (FILE *f,const event_t *e)
{
    
    
}

static void handle_repu_xmit_complete (const event_t *e)
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
    const repu_rendezvous_xfer_received_t *rtr =
        (const repu_rendezvous_xfer_received_t *)e;
    repu_target_t *tp = repu + rtr->target_num;
    tick_t write_start,write_complete;
    // repu_xmit_ack
    disk_write_start_t dws;
    tick_t write_variance =
    derived.chunk_disk_write_duration/config.write_variance;;
    tick_t write_duration = derived.chunk_disk_write_duration
                            - write_variance/2
                            +  (rand() % write_variance);
    
    assert(e);
    assert(rtr->target_num < derived.n_targets);
    assert(chunk_seq(rtr->cp));
    
    // send xmit_ack event
    
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

static  void log_repu_xmit_complete (FILE *f,const event_t *e)
{
    
}


#define MINIMUM_TCPV6_BYTES 74
#define TCP_CHUNK_SETUP_BYTES (3*MINIMUM_TCPV6_BYTES+200)
// 3 packets for TCP connectino setup plus minimal pre-transfer data
// The cluster_trip_time must still be added to this.

static void next_replica_xmit (chunkput_repu_t *cp,tick_t time_now)

// Schedule the next TCP transmit start after the previous tcp transmit for
// the same object has completed

{
    repu_xmit_start_t rxs;
    unsigned r;
    
    if (cp->replicas_unacked) {
        rxs.event.create_time = time_now;
        rxs.event.tllist.time = time_now + config.cluster_trip_time*3 +
        TCP_CHUNK_SETUP_BYTES*8;
        rxs.event.type = (event_type_t)REPU_XMIT_START;
        rxs.cp = (chunk_put_handle_t)cp;
        r = cp->repnum++;
        rxs.target_num = cp->ch_targets[r];
        insert_event(rxs);
    }
}

static void handle_repu_xmit_ack (const event_t *e)
{
    const repu_xmit_ack_t *rxa = (const repu_xmit_ack_t *)e;
    chunkput_repu_t *cp = (chunkput_repu_t *)rxa->cp;
    chunkput_t *new_cp;
    tick_t next_time;
    
    if (rxa->max_ongoing_rx > cp->max_ongoing_rx)
        cp->max_ongoing_rx  = rxa->max_ongoing_rx;
    next_time = e->tllist.time;
    next_time -= 3*config.cluster_trip_time;
    if (next_time <= now) next_time = now+1;
    if (++cp->acked < config.n_replicas)
        next_replica_xmit(cp,next_time);
    else if ((new_cp = next_cp(cp->cp.gateway,repucast_prot.cp_size)) != NULL)
        insert_next_chunk_put_ready(new_cp,next_time);
}

static void log_repu_xmit_ack (FILE *f,const event_t *e)
{
    
}

#define MAX_TALLY 2048
static void report_repu_chunk_distribution (FILE *f)

// Report distribution of chunks to targets to log_f

{
    unsigned tally [MAX_TALLY];
    const repu_target_t *tp;
    const repu_target_t *tp_lim;
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

protocol_t repucast_prot = {
    .tag = "repu",
    .name = "Replicast-Unicast",
    .cp_size = sizeof(chunkput_repu_t),
    .do_me = false,
    .init_target = init_repu_targets,
    .target = repu_target,
    .report_chunk_distribution = report_repu_chunk_distribution,
    .release_targets = release_repu_targets,
    .h = {
        {handle_repu_chunk_put_ready,log_repu_chunk_put_ready},
        {handle_repu_chunk_put_request_received,
            log_repu_chunk_put_request_received},
        {handle_repu_chunk_put_response_received,
            log_repu_chunk_put_response_received},
        {handle_repu_xmit_eligible,log_repu_xmit_eligible},
        {handle_repu_xmit_start,log_repu_xmit_start},
        {handle_repu_xmit_complete,log_repu_xmit_complete},
        {handle_repu_xmit_ack,log_repu_xmit_ack}
    }
};




