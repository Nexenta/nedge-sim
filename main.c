//
//  main.c
//  StorageClusterSim
//
//  Created by cait on 5/11/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

// List all of the assumptions in this model:
//
//      No processing time is assessed for unsolicited requests
//      Emphasis is on measuring network delays for payload transmission
//
//      The network is drop-free for payload transmissions. Neither TCP
//      or Replicast have to engagein retransmissions.
//
//      The network core is non-blocking. The Storage Cluster only has
//      to deal with or avoid congestion at the Target egress ports.
//
//      All ports are 10 GbE. Having all ports be the same speed is very
//      beneficial for TCP because a single transmitter will never cause
//      congestion at the destination.
//
//      The delays caused by PAUSE or Priority Pause frames are not modeled.
//
//      All chunks are the same size - for modeling simplicity.
//
// The goal of this simulation is to demonstrate the impacts of Target selection
// and the mostfundamental differences between Replicast and TCP. A separate
// simulation will detail the impact of Replicast congestion control versus
// conventional TCP congestion control for storage clusters.
//

#include "storage_cluster_sim.h"

sim_config_t config = {
    .n_negotiating_groups = N_NEGOTIATING_GROUPS,
    .n_targets_per_ng = N_TARGETS_PER_NG,
    .mbs_sec_per_target_drive = MBS_SEC_PER_TARGET_DRIVE,
    .n_replicas = N_REPLICAS,
    .chunk_size = CHUNK_SIZE,
    .chunks_per_object = CHUNKS_PER_OBJECT,
    .tracked_object_puts = TRACKED_OBJECT_PUTS,
    .cluster_utilization = CLUSTER_UTILIZATION
};

sim_derived_config_t derived;

static unsigned edepth = 0;

static event_t ehead = { // The list of events for gateways and targets
    .tllist = {
        .next = &ehead.tllist,
        .prev = &ehead.tllist,
        .time = ~0
    },
    .create_time = 0,
    .type = NULL_EVENT
};

#ifdef NDEBUG
#else
void tllist_node_verify(const tllist_t *t)
{
    assert(t);
    assert(t->next);
    assert(t->next->prev == t);
    assert(t->prev);
    assert(t->prev->next == t);
}

void tllist_verify(const tllist_t *t)
{
    const tllist_t *p;
    assert(t);
    assert(t->next);
    
    for (p = t->next; p != t->prev; p = p->next)
        assert (p->time <= p->next->time);
}
#endif

void tllist_remove (tllist_t *t)
{
    tllist_node_verify(t);
    t->prev->next = t->next;
    t->next->prev = t->prev;
}

static unsigned event_depth_by_type[NUM_EVENT_TYPES];

static void event_remove (event_t *e)
{
    assert(e != &ehead);
    assert(&e->tllist == ehead.tllist.next);
    --event_depth_by_type[e->type];
    tllist_remove(&e->tllist);
    free(e);
    assert(edepth);
    --edepth;
}

const tllist_t *tllist_find (const tllist_t *head,tick_t find)
//
// find the node which a new tllist_t entry with time 'find' should be
// inserted after. This may be the head itself.
//
{
    const tllist_t *p;
    
    tllist_node_verify(head);
    tllist_verify(head);
    for (p = head; p != head->prev; p = p->next) {
        tllist_node_verify(p);
        if (p->next->time > find)
            break;
    }
    return p;
}

void tllist_insert (tllist_t *ref,tllist_t *n)
//
// inset 'n' after 'ref'
//
{
    tllist_node_verify(ref);
    n->next = ref->next;
    n->prev = ref;
    ref->next->prev = n;
    ref->next = n;
    tllist_node_verify(n);
}

void __insert_event (event_t *n)
{
    tllist_t *after;
    
    assert(n);
    n->sig = 0x1234;
    after = (tllist_t *)tllist_find((tllist_t *)&ehead,n->tllist.time);
    tllist_insert (after,(tllist_t*)n);
    tllist_node_verify(after);
    tllist_node_verify((tllist_t *)n);
    tllist_verify((tllist_t *)&ehead);
    ++event_depth_by_type[n->type];
    ++edepth;
}

void _insert_event (const event_t *new_event,size_t event_size)
//
// calloc a copy of new_event with sub-type specific size
// insert this new node in the linked list based on new_event->trigger_time
//
{
    event_t *n = (event_t *)calloc(1,event_size);
    
    assert (n);
    memcpy(n,new_event,event_size);
    __insert_event(n);
}

static void log_event (const event_t *e)

// Output the event as a single text line in CSV format

{
    unsigned i;
    const object_put_ready_t *opr;
    const rep_chunk_put_ready_t   *cpr;
    const rep_chunk_put_request_received_t *cpreq;
    const rep_chunk_put_response_received_t *cpresp;
    const rep_chunk_put_accept_t *cpa;
    const rep_rendezvous_xfer_received_t *rtr;
    const tcp_xmit_received_t *txr;
    const tcp_reception_complete_t *trc;
    const disk_write_completion_t *dwc;
    const replica_put_ack_t *rpack;
    const chunk_put_ack_t *cpack;
    
    switch (e->type) {
        case OBJECT_PUT_READY:
            opr = (const object_put_ready_t *)e;
            fprintf(log_f,"%ld,%ld,OBJECT_PUT_READY,%d\n",
                    e->tllist.time,e->create_time,opr->n_chunks);
            break;
        case REP_CHUNK_PUT_READY:
            cpr = (const rep_chunk_put_ready_t *)e;
            fprintf(log_f,"%ld,%ld,CHUNK_PUT_READY,0x%lx,%d\n",
                    e->tllist.time,e->create_time,cpr->cp,chunk_seq(cpr->cp));
            break;
        case REP_CHUNK_PUT_REQUEST_RECEIVED:
            cpreq = (const rep_chunk_put_request_received_t *)e;
            fprintf(log_f,"%ld,%ld,REP_CHUNK_PUT_REQUEST_RECEIVED,0x%lx,%d,%d\n",
                    e->tllist.time,e->create_time,cpreq->cp,
                    chunk_seq(cpreq->cp),cpreq->target_num);
            break;
        case REP_CHUNK_PUT_RESPONSE_RECEIVED:
            cpresp = (const rep_chunk_put_response_received_t *)e;
            fprintf(log_f,
                    "%ld,%ld,REP_CHUNK_PUT_RESPONSE_RCVD,0x%lx,%d,%d,%ld,%ld\n",
                    e->tllist.time,e->create_time,cpresp->cp,
                    chunk_seq(cpresp->cp),cpresp->target_num,
                    cpresp->bid_start,cpresp->bid_lim);
            break;
        case REP_CHUNK_PUT_ACCEPT_RECEIVED:
            cpa = (const rep_chunk_put_accept_t *)e;

            fprintf(log_f,
                    "%ld,%ld,REP_CHUNK_PUT_ACCEPT_RECEIVED,0x%lx,%d,%d,%ld,%ld",
                    e->tllist.time,e->create_time,cpa->cp,chunk_seq(cpa->cp),
                    cpa->target_num,
                    cpa->window_start,cpa->window_lim);
            
            for (i=0;i != N_REPLICAS;++i)
                fprintf (log_f,",%d",cpa->accepted_target[i]);
            fprintf(log_f,"\n");
            break;
        case REP_RENDEZVOUS_XFER_RECEIVED:
            rtr = (const rep_rendezvous_xfer_received_t *)e;
            fprintf(log_f,"%ld,%ld,REP_CHUNK_RENDEZVOUS_XFER_RCVD,0x%lx,%d,%d\n",
                    e->tllist.time,e->create_time,rtr->cp,chunk_seq(rtr->cp),
                    rtr->target_num);
            break;
        case TCP_XMIT_RECEIVED:
            txr = (const tcp_xmit_received_t *)e;
            fprintf(log_f,"%ld,%ld,TCP_XMIT_RECEIVED,0x%lx,%d,%d\n",
                    e->tllist.time,e->create_time,txr->cp,chunk_seq(txr->cp),
                    txr->target_num);
            break;
        case TCP_RECEPTION_COMPLETE:
            trc = (const tcp_reception_complete_t *)e;
            fprintf(log_f,"%ld,%ld,TCP_RECEPTION_COMPLETE,0x%lx,%d,%d\n",
                    e->tllist.time,e->create_time,trc->cp,chunk_seq(trc->cp),
                    trc->target_num);
            break;
        case DISK_WRITE_COMPLETION:
            dwc = (const disk_write_completion_t *)e;
            fprintf(log_f,"%ld,%ld,DISK_WRITE_COMPLETION,0x%lx,%d,%d\n",
                    e->tllist.time,e->create_time,dwc->cp,chunk_seq(dwc->cp),
                    dwc->target_num);
            break;
        case REPLICA_PUT_ACK:
            rpack = (const replica_put_ack_t *)e;
            fprintf(log_f,"%ld,%ld,REPLICA_PUT_ACK,0x%lx,%d,%d\n",
                    e->tllist.time,e->create_time,rpack->cp,
                    chunk_seq(rpack->cp),rpack->target_num);
            break;
        case CHUNK_PUT_ACK:
            cpack = (const chunk_put_ack_t *)e;
            fprintf(log_f,"%ld,%ld,CHUNK_PUT_ACK,0x%lx\n",
                    e->tllist.time,e->create_time,cpack->cp);
            break;
        case NULL_EVENT:
        case NUM_EVENT_TYPES:
            break;
    }
}

static void insert_next_put (tick_t insert_time)
{
    object_put_ready_t new_put;
    
    new_put.event.tllist.time = new_put.event.create_time = insert_time;
    new_put.n_chunks = CHUNKS_PER_OBJECT;
    new_put.event.type = OBJECT_PUT_READY;
    insert_event(new_put);
}

tick_t now = 0;

static unsigned n_tracked_completions = 0;

static unsigned n_chunk_puts = 0;

static void process_event (const event_t *e)
{
    now = e->tllist.time;
    switch (e->type) {
        case OBJECT_PUT_READY:
            handle_object_put_ready (e);
            break;
        case REP_CHUNK_PUT_READY:
            handle_chunk_put_ready (e);
            ++n_chunk_puts;
            break;
        case REP_CHUNK_PUT_REQUEST_RECEIVED:
            handle_rep_chunk_put_request_received(e);
            break;
        case REP_CHUNK_PUT_RESPONSE_RECEIVED:
            handle_rep_chunk_put_response_received(e);
            break;
        case REP_CHUNK_PUT_ACCEPT_RECEIVED:
            handle_rep_chunk_put_accept_received(e);
            break;
        case REP_RENDEZVOUS_XFER_RECEIVED:
            handle_rep_rendezvous_xfer_received(e);
            break;
        case TCP_XMIT_RECEIVED:
            handle_tcp_xmit_received(e);
            break;
        case TCP_RECEPTION_COMPLETE:
            handle_tcp_reception_complete(e);
            break;
        case DISK_WRITE_COMPLETION:
            handle_disk_write_completion(e);
            break;
        case REPLICA_PUT_ACK:
            handle_replica_put_ack(e);
            break;
        case CHUNK_PUT_ACK:
            if (handle_chunk_put_ack(e)) {
                ++n_tracked_completions;
            }
            break;
        case NULL_EVENT:
        case NUM_EVENT_TYPES:
            break;
    }
}

static void simulate (void)
{
    const event_t *e;

    unsigned delta;
    unsigned put_seed = 0x12345678;

    tick_t next_object_put_event = rand()%(2*derived.ticks_per_object);

    next_object_put_event = rand_r(&put_seed) % (2*derived.ticks_per_object);
    
    printf("Total %dKB Chunks %d.\n",config.chunk_size/1024,
           derived.n_tracked_puts);
    e = (const event_t *)ehead.tllist.next;
    
    for (n_tracked_completions = 0;
         n_tracked_completions < derived.n_tracked_puts;)
    {
        tllist_verify((const tllist_t *)&ehead);
        if (next_object_put_event < e->tllist.time) {
            insert_next_put(next_object_put_event);
            e = (const event_t *)ehead.tllist.next;
            delta = rand_r(&put_seed)%(2*derived.ticks_per_object) + 1;
            next_object_put_event += delta;
        }
        assert(e->sig == 0x1234);
        process_event(e);
        if (log_f) log_event(e);
        event_remove((event_t *)e);
        tllist_verify((const tllist_t *)&ehead);
        e = (const event_t *)ehead.tllist.next;
    }
    report_duration_stats();
    while (e != (const event_t *)&ehead) {
        process_event(e);
        event_remove((event_t *)e);
        e = (const event_t *)ehead.tllist.next;
    }
    (void)now;
}

#define UDP_SIZE_BYTES 9000
#define UDP_OVERHEAD_BYTES 60
// FIXME: refine above

static void derive_config (void)
{
    unsigned chunk_udp_packets;
    tick_t   j;
    
    derived.n_targets = config.n_negotiating_groups * config.n_targets_per_ng;
    derived.n_tracked_puts = config.chunks_per_object * config.tracked_object_puts;
    derived.total_write_mbs =
        derived.n_tracked_puts * config.chunk_size / 1024L /1024L;
    derived.disk_kb_write_time =
        (unsigned)((TICKS_PER_SECOND/1024L)/config.mbs_sec_per_target_drive);
    chunk_udp_packets = (config.chunk_size + UDP_SIZE_BYTES - 1)/UDP_SIZE_BYTES;
    derived.chunk_xmit_duration =
        (config.chunk_size+UDP_OVERHEAD_BYTES*chunk_udp_packets)*8L;
    derived.chunk_disk_write_duration =
        divup(config.chunk_size,1024)*derived.disk_kb_write_time;
    j = derived.chunk_disk_write_duration * config.n_replicas *
        config.chunks_per_object;
    j = divup(j,derived.n_targets);
    derived.ticks_per_object = divup(j*100L,config.cluster_utilization);
}

static FILE *open_outf (const char *type)
{
    auto char name[66];
    
    assert(strlen(type) <= 20);

    sprintf(name,"%s.csv",type);
    return fopen(name,"w");
}

//
// TODO:
// This should be invoked to produce N chunks in bursts of chunks
// at a configurable (and potentially dynamically adjusted) rate.
//
// the output will be to stdout from the log_event() routine.
// the output is not intended for a human to read, but to be processed
// by the next program in a pipeline (whether piped or by running another
// program to read the output from this program)
//
// Currently it is assumed that this sourcefile will be made with
// #REPLICAST defined for one executable and once without
//

bool replicast; // simulation is currently in replicast mode

FILE *log_f;
FILE *bid_f;

int main(int argc, const char * argv[]) {

    
    // TODO: accept command line customization of config
    derive_config();
    bool nr_enabled = true;
    log_f = open_outf("log");
    bid_f = open_outf("bid");

    fprintf(log_f,"Simulating Replicast\n");
    fprintf(bid_f,"Simulating Replicast\n");
    replicast = true;
    init_rep_targets(derived.n_targets);
    simulate();
    release_rep_targets();
    if (nr_enabled) {
        fprintf(log_f,"Simulating Non-replicast\n");
        fprintf(bid_f,"Simulating Non-replicast\n");
        replicast = false;
        init_nonrep_targets(derived.n_targets);
        simulate();
        release_nonrep_targets();
    }
    fclose(log_f);
    fclose(bid_f);
    exit(0);
}
