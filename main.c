//
//  main.c
//  StorageClusterSim
//
//  Created by Caitlin Bestler on 7/9/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

// List of assumptions made for this model:
//
//      No processing time is assessed for unsolicited requests
//      Emphasis is on measuring network delays for payload transmission
//
//      The network is drop-free for payload transmissions. Neither TCP
//      or Replicast have to engagein retransmissions.
//
//      Class-of-service network features protects storage payload from
//      other traffic (including unsolicited commands and other TCP traffic).
//
//      The network core is non-blocking. The Storage Cluster only has
//      to deal with or avoid congestion at the Target egress ports. Of course
//      it "deals with" congestion on in ingress port in that no server can
//      transmit two ethernet frames at the same time.
//
//      All ports are 10 GbE. Having all ports be the same speed is very
//      beneficial for TCP because a single transmitter will never cause
//      congestion at the destination.
//
//      The delays caused by PAUSE or Priority Pause frames are not modeled.
//      When other congestion control is working these frames will be used
//      very rarely.
//
//      All chunks are the same size - for modeling simplicity.
//
//      Any benefits from deduplication are not being considered. Benefits can
//      be sustantial for certain workloads, but those benefits should be
//      identical for both Replicast and Consistent Hashing.
//
// The goal of this simulation is to demonstrate the impacts of Target selection
// and the mostfundamental differences between Replicast and TCP. A separate
// simulation is needed to detail the impact of Replicast congestion control
// versus conventional TCP congestion control for storage clusters.
//

#include "storage_cluster_sim.h"

sim_config_t config = {
    .cluster_trip_time = CLUSTER_TRIP_TIME,
    .n_negotiating_groups = N_NEGOTIATING_GROUPS,
    .n_targets_per_ng = N_TARGETS_PER_NG,
    .mbs_sec_per_target_drive = MBS_SEC_PER_TARGET_DRIVE,
    .n_replicas = N_REPLICAS,
    .chunk_size = CHUNK_SIZE,
    .n_gateways = N_GATEWAYS,
    .per_gateway_limit=PER_GATEWAY_LIMIT,
    .sim_duration = 1024L*1024L*1024L,
    .seed = 0x12345678
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

gateway_t gateway[MAX_GATEWAYS];

trackers_t track = {.min_duration = ~0L};

static unsigned tllist_len (const tllist_t *head)
{
    unsigned n;
    const tllist_t *p;
    
    for (n=0,p=head->next;p != head;p = p->next,++n)
        ;
    return n;
}

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
    assert(edepth);
    assert(e);

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
    assert(n->type != NULL_EVENT);
    n->sig = 0x1234;
    after = (tllist_t *)tllist_find((tllist_t *)&ehead,n->tllist.time);
    tllist_insert (after,(tllist_t*)n);
    tllist_node_verify(after);
    tllist_node_verify((tllist_t *)n);
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
    assert(edepth == tllist_len(&ehead.tllist));
}

static void log_event (FILE *f,const event_t *e)

// Output the event as a single text line in CSV format to 'f'

{
    unsigned i;
    const char *tag = replicast ? "rep" : "non";
    union event_ptr_union {
        const event_t *e;
        const gateway_ready_t *gr;
        const chunk_put_ready_t   *cpr;
        const rep_chunk_put_request_received_t *cpreq;
        const rep_chunk_put_response_received_t *cpresp;
        const rep_chunk_put_accept_t *cpa;
        const rep_rendezvous_xfer_received_t *rtr;
        const tcp_xmit_received_t *txr;
        const tcp_reception_complete_t *trc;
        const tcp_reception_ack_t *tra;
        const disk_write_start_t *dws;
        const disk_write_completion_t *dwc;
        const replica_put_ack_t *rpack;
        const chunk_put_ack_t *cpack;
    } u;
    
    u.e = e;
    switch (e->type) {
        case GATEWAY_READY:
            fprintf(f,"0x%lx,0x%lx,%s GATEWAY_READY,%d\n",
                    e->tllist.time,e->create_time,tag,u.gr->gateway);
            break;
        case CHUNK_PUT_READY:
            fprintf(f,"0x%lx,0x%lx,%s CHUNK_PUT_READY,0x%lx,%d,gw,%d\n",
                    e->tllist.time,e->create_time,tag,
                    u.cpr->cp,chunk_seq(u.cpr->cp),chunk_gateway(u.cpr->cp));
            break;
        case REP_CHUNK_PUT_REQUEST_RECEIVED:
            fprintf(f,"0x%lx,0x%lx,REP_CHUNK_PUT_REQUEST_RECEIVED,0x%lx,%d,%d\n",
                    e->tllist.time,e->create_time,u.cpreq->cp,
                    chunk_seq(u.cpreq->cp),u.cpreq->target_num);
            break;
        case REP_CHUNK_PUT_RESPONSE_RECEIVED:
            fprintf(f,"0x%lx,0x%lx,REP_CHUNK_PUT_RESPONSE_RCVD,0x%lx,%d,tgt,%d,",
                    e->tllist.time,e->create_time,u.cpresp->cp,
                    chunk_seq(u.cpresp->cp),u.cpresp->target_num);
            fprintf(f,"0x%lx,0x%lx\n",u.cpresp->bid_start,u.cpresp->bid_lim);
            break;
        case REP_CHUNK_PUT_ACCEPT_RECEIVED:
            fprintf(f,
                    "0x%lx,0x%lx,REP_CHUNK_PUT_ACCEPT_RECEIVED,0x%lx,%d,CP,%d,",
                    e->tllist.time,e->create_time,
                    u.cpa->cp,chunk_seq(u.cpa->cp),
                    u.cpa->target_num);
            fprintf(f,"%ld,%ld,targets,",u.cpa->window_start,u.cpa->window_lim);
            for (i=0;i != N_REPLICAS;++i)
                fprintf (f,",%d",u.cpa->accepted_target[i]);
            fprintf(f,"\n");
            break;
        case REP_RENDEZVOUS_XFER_RECEIVED:
            fprintf(f,"0x%lx,0x%lx",e->tllist.time,e->create_time);
            fprintf(f,",REP_CHUNK_RENDEZVOUS_XFER_RCVD,CP,0x%lx,%d,tgt,%d\n",
                    u.rtr->cp,chunk_seq(u.rtr->cp),u.rtr->target_num);
            break;
        case TCP_XMIT_RECEIVED:
            fprintf(f,"0x%lx,0x%lx,non TCP_XMIT_RECEIVED,0x%lx,%d,tgt,%d\n",
                    e->tllist.time,e->create_time,
                    u.txr->cp,chunk_seq(u.txr->cp),
                    u.txr->target_num);
            break;
        case TCP_RECEPTION_COMPLETE:
            fprintf(f,"0x%lx,0x%lx,non TCP_RECEPTION_COMPLETE,0x%lx,%d,tgt,%d\n",
                    e->tllist.time,e->create_time,
                    u.trc->cp,chunk_seq(u.trc->cp),
                    u.trc->target_num);
            break;
        case TCP_RECEPTION_ACK:
            fprintf(f,"0x%lx,0x%lx,non TCP_RECEPTION_ACK,0x%lx,%d,tgt,%d\n",
                    e->tllist.time,e->create_time,
                    u.tra->cp,chunk_seq(u.tra->cp),u.tra->target_num);
            break;
        case DISK_WRITE_START:
            fprintf(f,"0x%lx,0x%lx,%s DISK_WRITE_START,0x%lx,%d",
                    e->tllist.time,e->create_time,tag,
                    u.dws->cp,chunk_seq(u.dws->cp));
            fprintf(f,",tgt,%d,qdepth,%d\n",u.dwc->target_num,
                    u.dwc->write_qdepth);
            break;
        case DISK_WRITE_COMPLETION:
            fprintf(f,"0x%lx,0x%lx,%s DISK_WRITE_COMPLETION,0x%lx,%d,tgt,%d",
                    e->tllist.time,e->create_time,tag,
                    u.dwc->cp,chunk_seq(u.dwc->cp),
                    u.dwc->target_num);
            fprintf(f,",target,%d,qdepth,%d\n",u.dwc->target_num,
                    u.dwc->write_qdepth);
            break;
        case REPLICA_PUT_ACK:
            fprintf(f,"0x%lx,0x%lx,%s REPLICA_PUT_ACK,0x%lx,%d,tgt,%d",
                    e->tllist.time,e->create_time,tag,u.rpack->cp,
                    chunk_seq(u.rpack->cp),u.rpack->target_num);
            fprintf(f,",depth,%d,gw,%d\n",u.rpack->write_qdepth,
                    chunk_gateway(u.rpack->cp));
            break;
        case CHUNK_PUT_ACK:
	    assert(u.cpack->write_qdepth >= 0);
            fprintf(f,"0x%lx,0x%lx,%s CHUNK_PUT_ACK,0x%lx,depth,%d,gw,%d\n",
                    e->tllist.time,e->create_time,tag,u.cpack->cp,
                    u.cpack->write_qdepth,chunk_gateway(u.cpack->cp));
            break;
        case NULL_EVENT:
        case NUM_EVENT_TYPES:
            assert(false);
            break;
        case TRACK_SAMPLE:
            fprintf(f,"0x%lx,0x%lx,TRACK_SAMPLE\n", e->tllist.time,
                    e->create_time);
            break;
    }
}

tick_t now = 0;

bool replicast; // simulation is currently in replicast mode

#define MSEC_TICKS (TICKS_PER_SECOND/1000L)

static void track_report (void)
{
    const char *tag = replicast ? "replicast" : "non";
    
    fprintf(log_f,"%s,track@,0x%lx,%lu,%lu,%lu,%lu,active-targets,%u\n",tag,now,
            track.n_initiated,track.n_writes_jnitiated,track.n_writes_completed,
            track.n_completions,track.n_active_targets);
}

static void process_event (const event_t *e)

// process a single event

{
    track_sample_t track_it;
    
    now = e->tllist.time;
    switch (e->type) {
        case GATEWAY_READY:
            handle_gateway_ready (e);
            break;
        case CHUNK_PUT_READY:
            handle_chunk_put_ready (e);
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
        case TCP_RECEPTION_ACK:
            handle_tcp_reception_ack(e);
            break;
        case DISK_WRITE_START:
            handle_disk_write_start(e);
            break;
        case DISK_WRITE_COMPLETION:
            handle_disk_write_completion(e);
            break;
        case REPLICA_PUT_ACK:
            handle_replica_put_ack(e);
            break;
        case CHUNK_PUT_ACK:
            handle_chunk_put_ack(e);
            break;
        case TRACK_SAMPLE:
            track_report();
            if (now < config.sim_duration) {
                track_it.event.create_time = now;
                track_it.event.tllist.time = now + TICKS_PER_SECOND / 1000;
                track_it.event.type = TRACK_SAMPLE;
                insert_event(track_it);
            }
            break;
        case NULL_EVENT:
        case NUM_EVENT_TYPES:
            assert(false);
            break;
    }
}

static void simulate (bool do_replicast)
{
    const event_t *e;
    track_sample_t track_it;
    unsigned i;
    tick_t first_chunk_time = 0L;
    
    gateway[0].credit = (signed)config.per_gateway_limit;
    gateway[0].pending_cp = (void *)0;
    memcpy(gateway+1,gateway,(sizeof gateway)-(sizeof gateway[0]));
    
    track_it.event.create_time = now = 0L;
    track_it.event.tllist.time = TICKS_PER_SECOND / 1000;
    track_it.event.type = TRACK_SAMPLE;

    insert_event(track_it);
    
    replicast = do_replicast;
    srand(config.seed+1);

    printf("\n%d Gateways putting %dKB Chunks.\n",config.n_gateways,
           config.chunk_size/1024);
    e = (const event_t *)ehead.tllist.next;
    
    for (i = 0; i != config.n_gateways; ++i) {
        first_chunk_time += (rand() % 1024);
        insert_next_put(i,first_chunk_time);
    }
    
    for (;
         now < config.sim_duration;
         e = (const event_t *)ehead.tllist.next)
    {
        if (log_f) log_event(log_f,e);
        process_event(e);
        event_remove((event_t *)e);
        tllist_verify((const tllist_t *)&ehead);
    }
    report_duration_stats();
    while (e != (const event_t *)&ehead) {
        assert(e->sig == 0x1234);
        assert (e != &ehead);
        assert (e->type != NULL_EVENT);
        if (e->type != TRACK_SAMPLE)
            process_event(e);
        event_remove((event_t *)e);
        e = (const event_t *)ehead.tllist.next;
    }
}

#define ETH_SIZE_BYTES 9000
#define MINIMUM_UDPV6_BYTES 66
#define UDP_SIZE_BYTES (ETH_SIZE_BYTES-MINIMUM_UDPV6_BYTES)
#define MINIMUM_TCPV6_BYTES 74
#define TCP_SIZE_BYTES (ETH_SIZE_BYTES-MINIMUM_TCPV6_BYTES)


static void derive_config (void)
{
    unsigned chunk_udp_packets;
    unsigned chunk_tcp_packets;
    
    derived.n_targets = config.n_negotiating_groups * config.n_targets_per_ng;
    derived.total_write_mbs =
        divup(derived.n_tracked_puts * config.chunk_size,1024L*1024L);
    derived.disk_kb_write_time =
        (unsigned)((TICKS_PER_SECOND/1024L)/config.mbs_sec_per_target_drive);
    chunk_udp_packets = divup(config.chunk_size,UDP_SIZE_BYTES);
    derived.chunk_udp_xmit_duration =
        (config.chunk_size+MINIMUM_UDPV6_BYTES*chunk_udp_packets)*8L;
    chunk_tcp_packets = divup(config.chunk_size,TCP_SIZE_BYTES);
    derived.chunk_tcp_xmit_duration =
        (config.chunk_size+MINIMUM_TCPV6_BYTES*chunk_tcp_packets)*8L;
    
    derived.chunk_disk_write_duration =
        divup(config.chunk_size,1024)*derived.disk_kb_write_time;
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

FILE *log_f;
FILE *bid_f;

static void usage (const char *progname) {
    fprintf(stderr,"Usage: %s",progname);
    fprintf(stderr," [rep|ch]");
    fprintf(stderr," [duration <msecs>]");
    fprintf(stderr," [ngs <#>]");
    fprintf(stderr," [targets_per <#>]");
    fprintf(stderr," [chunk_size <kbytes>]\n");
    fprintf(stderr," [gateways <#>],");
    fprintf(stderr," [mbs_sec <#>");
    fprintf(stderr," penalty <ticks_per_chunk>");
    fprintf(stderr," [cluster_trip_time <ticks>\n");
    fprintf(stderr,"\nOr %s help\n",progname);
    fprintf(stderr,"    to print this.\n");

    fprintf(stderr,"\nPenalty is assessed per chunk for Replicast overhead.\n");
    fprintf(stderr,"\nrep does replicast only.\n");
    fprintf(stderr,"ch does consistent hash only.\n");
    fprintf(stderr,"Default is to do both\n");
}

static void log_config (FILE *f)
{ // TODO add missing fields
    float duration;
    fprintf(f,"config.do_replicast:%d\n",config.do_replicast);
    fprintf(f,"config.do_ch:%d\n",config.do_ch);
    duration = (float)config.sim_duration * 1000 / TICKS_PER_SECOND;
    fprintf(f,"config.sim_duration:%lu (0x%lx) ticks = %.2f msecs\n",config.sim_duration, config.sim_duration, duration);
    fprintf(f,"config.cluster_trip_time:%d\n",config.cluster_trip_time);
    fprintf(f,"confg.chunk_size:%d\n",config.chunk_size);
    fprintf(f,"config.mbs_sec_per_target_drive:%d\n",
            config.mbs_sec_per_target_drive);
    fprintf(f,"config.n_negotiating_groups:%d\n",config.n_negotiating_groups);
    fprintf(f,"config.n_replicas:%d\n",config.n_replicas);
    fprintf(f,"config.n_targets_per_ng:%d\n",config.n_targets_per_ng);
    fprintf(f,"config.n_gateways:%d\n",config.n_gateways);
    fprintf(f,"config.penalty:%u\n",config.replicast_packet_processing_penalty);
    fprintf(f,"config.seed:%d\n",config.seed);
    fprintf(f,"config.replicast_packet_processing_penalty:%d\n",
            config.replicast_packet_processing_penalty);
}

static void customize_config (int argc, const char ** argv)
{ // TODO add new fields
    const char *argv0 = argv[0];
    
    config.do_replicast = config.do_ch = true;
    
    if (argc == 2) {
        usage(argv0);
        exit(1);
    }
    for (--argc,++argv;argc >= 2;argv+=2,argc-=2) {
        if (0 == strcmp(*argv,"ngs"))
            config.n_negotiating_groups = atoi(argv[1]);
        else if (0 == strcmp(*argv,"targets_per"))
            config.n_targets_per_ng = atoi(argv[1]);
        else if (0 == strcmp(*argv,"chunk_size"))
            config.chunk_size = atoi(argv[1])*1024;
        else if (0 == strcmp(*argv,"gateways")) {
            config.n_gateways = atoi(argv[1]);
            if (config.n_gateways > MAX_GATEWAYS) {
                fprintf(stderr,"Gateways set to max:%d\n",MAX_GATEWAYS);
                config.n_gateways = MAX_GATEWAYS;
            }
        }
        else if (0 == strcmp(*argv,"duration"))
            config.sim_duration = atoi(argv[1]) * (TICKS_PER_SECOND/1000);
        else if (0 == strcmp(*argv,"seed"))
            config.seed = atoi(argv[1]);
        else if (0 == strcmp(*argv,"mbs_sec"))
            config.mbs_sec_per_target_drive = atoi(argv[1]);
        else if (0 == strcmp(*argv,"cluster_trip_time"))
            config.cluster_trip_time = atoi(argv[1]);
        else if (0 == strcmp(*argv,"rep")) {
            config.do_replicast = true;
            config.do_ch = false;
            --argv,--argc;
        }
        else if (0 == strcmp(*argv,"ch")) {
            config.do_replicast = false;
            config.do_ch = true;
            --argv,--argc;
        }
        else if (0 == strcmp(*argv,"penalty"))
            config.replicast_packet_processing_penalty = atoi(argv[1]);
        else {
            usage(argv0);
            exit(1);
        }
    }
}

int main(int argc, const char * argv[]) {
    customize_config(argc,argv);
    derive_config();
    log_f = open_outf("log");
    bid_f = open_outf("bid");

    if (config.do_replicast) {
        printf("\n\nSimulating Replicast\n");
        fprintf(log_f,"Simulating Replicast\n");
        fprintf(bid_f,"Simulating Replicast\n");

        log_config(log_f);
    
        replicast = true;
        init_rep_targets(derived.n_targets);
        simulate(true);
        report_rep_chunk_distribution();
        release_rep_targets();
    }
    if (config.do_ch) {
        printf("\n\nSimulating Non-replicast\n");
        fprintf(log_f,"Simulating Non-replicast\n");
        fprintf(bid_f,"Simulating Non-replicast\n");

        init_seqnum();
        replicast = false;
        init_nonrep_targets(derived.n_targets);
        simulate(false);
        report_nonrep_chunk_distribution();
        release_nonrep_targets();
    }
    fclose(log_f);
    fclose(bid_f);
    exit(0);
}
