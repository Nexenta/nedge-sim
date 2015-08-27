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
    .mbs_per_target_drive = MBS_PER_TARGET_DRIVE,
    .gateway_mbs = GATEWAY_MBS,
    .n_replicas = N_REPLICAS,
    .chunk_size = CHUNK_SIZE,
    .n_gateways = N_GATEWAYS,
    .sim_duration = TICKS_PER_SECOND/100,
    .bid_window_multiplier_pct = BID_WINDOW_MULTIPLIER_PCT,
    .write_variance = 50,
    .sample_interval = SAMPLE_INTERVAL,
    .seed = 0x12345678,
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

trackers_t track = {.min_duration = ~0L};
static trackers_t track_prev = { 0 };

#ifdef NDEBUG
#else
static unsigned tllist_len (const tllist_t *head)
{
    unsigned n;
    const tllist_t *p;
    
    for (n=0,p=head->next;p != head;p = p->next,++n)
        ;
    return n;
}

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

static void log_event (FILE *f,const protocol_t *sp,const event_t *e)

// Output the event as a single text line in CSV format to 'f'

{
    union event_ptr_union {
        const event_t *e;
        const chunk_put_ready_t   *cpr;
        const rep_chunk_put_request_received_t *cpreq;
        const rep_chunk_put_response_received_t *cpresp;
        const tcp_xmit_received_t *txr;
        const tcp_reception_complete_t *trc;
        const tcp_reception_ack_t *tra;
        const disk_write_start_t *dws;
        const disk_write_completion_t *dwc;
        const replica_put_ack_t *rpack;
        const chunk_put_ack_t *cpack;
    } u;
    
    u.e = e;

    if (e->type == DISK_WRITE_START) {
        if (!config.terse) {
            unsigned tgt = u.dws->target_num;
            target_t *tp = sp->target(tgt);

            fprintf(f,"0x%lx,0x%lx,%s DISK_WRITE_START,0x%lx,%d",
                    e->tllist.time,e->create_time,protocol->tag,
                    u.dws->cp,chunk_seq(u.dws->cp));
            fprintf(f,",tgt,%d,inflight,%d\n",u.dwc->target_num,
                    tp->total_inflight);
        }
    }
    else if (e->type == DISK_WRITE_COMPLETION) {
        if (!config.terse) {
            unsigned tgt = u.dwc->target_num;
            target_t *tp = protocol->target(tgt);
            
            fprintf(f,"0x%lx,0x%lx,%s DISK_WRITE_COMPLETION,0x%lx,%d,tgt,%d",
                    e->tllist.time,e->create_time,protocol->tag,
                    u.dwc->cp,chunk_seq(u.dwc->cp),
                    u.dwc->target_num);
            fprintf(f,",target,%d,inflight,%d\n",u.dwc->target_num,
                    tp->total_inflight);
        }
    }
    else if (e->type == REPLICA_PUT_ACK) {
        if (!config.terse) {
            unsigned tgt = u.rpack->target_num;
            target_t *tp = protocol->target(tgt);
            
            fprintf(f,"0x%lx,0x%lx,%s REPLICA_PUT_ACK,CP,0x%lx,%d,tgt,%d",
                    e->tllist.time,e->create_time,protocol->tag,u.rpack->cp,
                    chunk_seq(u.rpack->cp),u.rpack->target_num);
            fprintf(f,",inflight,%d\n",tp->total_inflight);
        }
    }
    else if (e->type == CHUNK_PUT_ACK) {
        if (!config.terse) {
            fprintf(f,"0x%lx,0x%lx,%s CHUNK_PUT_ACK,CP,0x%lx,%d\n",
                    e->tllist.time,e->create_time,protocol->tag,u.cpack->cp,
                    chunk_seq(u.cpack->cp));
        }
    }
    else if (TRANSPORT_EVENT_BASE <= e->type  &&
             e->type < TRANSPORT_EVENT_BASE+NUM_PROTOCOL_SPECIFIC_EVENTS)
        sp->h[e->type - TRANSPORT_EVENT_BASE].log_func(f,e);
}

tick_t now = 0;

FILE *log_f;
FILE *bid_f;
static FILE *inflight_f;

#define MSEC_TICKS (TICKS_PER_SECOND/1000L)

#define for_ng(target,ng) \
for ((target) = (ng);\
    (target) < derived.n_targets;\
    (target) += config.n_negotiating_groups)

static void inflight_report (FILE *f,const char *tag)
{
    unsigned t,sum,min,max;
    float avg;
    unsigned max_chunks_per_target =
        (unsigned)(now / derived.chunk_disk_write_duration);
    target_t *tp;
    
    fprintf(f,"%s,now,0x%lx,max_chunks,%d,targets",
            tag,now,max_chunks_per_target);
    sum = max = avg = 0; min = 999999;
    for (t = 0; t != derived.n_targets; ++t) {
        tp = protocol->target(t);

        fprintf(f,",%d",tp->total_inflight);
        sum += tp->total_inflight;
	if (min > tp->total_inflight) min = tp->total_inflight;
	if (max < tp->total_inflight) max = tp->total_inflight;
    }
    avg = (float)sum/derived.n_targets;
    if (avg) {
	fprintf(f,"\nper-target chunks-in-progress: min %d (%.2f * avg) ",
	    min,((float)min)/avg);
	fprintf(f," average %3.1f max %d (%.2f * avg)\n",
	    avg,max,((float)max)/avg);
    }
}

static void track_report (FILE *inflight_f)
{
    unsigned now_millis = (unsigned)divup(now, TICKS_PER_SECOND/1000);
    unsigned completed_since_prev_report = (unsigned)
        (track.n_completions - track_prev.n_completions);
    unsigned active_target_pct =
        divup(track.n_active_targets * 100, derived.n_targets);

    fprintf(log_f,
            "%s,track@,0x%lx,%u,%lu,%lu,%lu,%lu,%u,active-targets,%u,%u%%\n",
            protocol->tag,now,now_millis,
            track.n_initiated,track.n_writes_initiated,track.n_writes_completed,
            track.n_completions,completed_since_prev_report,
            track.n_active_targets, active_target_pct);

    fflush(log_f);

    if (now > 0)
    	inflight_report(inflight_f,protocol->tag);

    memcpy(&track_prev, &track, sizeof(track));
}

static void process_event (const protocol_t *sp,const event_t *e)

// process a single event

{
    track_sample_t track_it;
    
    if (e->type != TRACK_SAMPLE) {
        assert (e->tllist.time >= now);
        now = e->tllist.time;
    }
    if (TRANSPORT_EVENT_BASE <= e->type  &&
             e->type < TRANSPORT_EVENT_BASE+NUM_PROTOCOL_SPECIFIC_EVENTS)
        sp->h[e->type -TRANSPORT_EVENT_BASE].handle_func(e);
    else if (e->type == DISK_WRITE_START)
        handle_disk_write_start(e);
    else if (e->type == DISK_WRITE_COMPLETION)
        handle_disk_write_completion(e);
    else if (e->type == REPLICA_PUT_ACK)
        handle_replica_put_ack(e);
    else if (e->type == CHUNK_PUT_ACK)
        handle_chunk_put_ack(e);
    else if (e->type == TRACK_SAMPLE) {
        track_report(inflight_f);
        if (now < config.sim_duration) {
            track_it.event.create_time = e->tllist.time;
            track_it.event.tllist.time = e->tllist.time +
            config.sample_interval;
            track_it.event.type = TRACK_SAMPLE;
            insert_event(track_it);
        }
    }
    else
        assert(false);
}

static void start_gateway_thread (unsigned num,size_t size)

// replicast: create the first chunk_put_ready for this object, post it
// once it is scheduled the next chunk_put_request can be sent.

{
    chunk_put_ready_t cpr;
    gateway_t *gateway = calloc(1,sizeof *gateway);
    chunkput_t *cp;
    
    assert(gateway);
    gateway->num = num;
    gateway->n_chunks = 0;
    cp = next_cp(gateway,size);
    
    assert(cp);
    assert(!cp->mbz);
    
    cpr.event.create_time = now;
    cpr.event.tllist.time = (tick_t)num;
    cpr.event.type = CHUNK_PUT_READY;
    cpr.cp = (chunk_put_handle_t)cp;
    
    insert_event (cpr);
}

const protocol_t *protocol;

static void simulate (const protocol_t *sp)
{
    const event_t *e;
    track_sample_t track_it;
    unsigned i;
    unsigned prior_tenths_done = 0,tenths_done;
    
    assert(sp);
    protocol = sp;
    sp->init_target(derived.n_targets);
    track.max_tracked = 250000; // TODO base this on duration / max_rate
    track.durations = calloc(sizeof *track.durations,track.max_tracked);
    
    track_it.event.create_time = now = 0L;
    track_it.event.tllist.time = config.sample_interval;
    track_it.event.type = TRACK_SAMPLE;
    insert_event(track_it);

    srand(config.seed+1);

    printf("\n%d Gateways (@%d MBS each) putting %d replicas of %dKB Chunks",
           config.n_gateways,config.gateway_mbs,config.n_replicas,
           config.chunk_size/1024);
    printf(" to %d targets (@%d MBS each) using protocol %s\n",
           derived.n_targets,config.mbs_per_target_drive,sp->name);
    
    e = (const event_t *)ehead.tllist.next;
    
    for (i = 0; i != config.n_gateways; ++i)
        start_gateway_thread(i,sp->cp_size);
    
    for (now = 0L;
         now < config.sim_duration;
         e = (const event_t *)ehead.tllist.next)
    {
        tenths_done = (unsigned)(now*10L/config.sim_duration);
        if (tenths_done != prior_tenths_done) {
            printf("-----%d0", tenths_done);
	    fflush(stdout);
            prior_tenths_done = tenths_done;
        }
        if (log_f) log_event(log_f,sp,e);
        process_event(sp,e);
        event_remove((event_t *)e);
        tllist_verify((const tllist_t *)&ehead);
    }
    report_duration_stats();
    track.drain = true;
    while (e != (const event_t *)&ehead) {
        assert(e->sig == 0x1234);
        assert (e != &ehead);
        assert (e->type != NULL_EVENT);
        if (e->type != TRACK_SAMPLE)
            process_event(sp,e);
        event_remove((event_t *)e);
        e = (const event_t *)ehead.tllist.next;
    }
    printf("Post-drain:");
    report_duration_stats();
    free(track.durations);
    memset(&track,0,sizeof(trackers_t));
    memset(&track_prev,0,sizeof(trackers_t));
    track.min_duration = ~0L;
    sp->report_chunk_distribution(log_f);
    sp->release_targets();
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
    unsigned gateway_xmit_time;
    
    derived.n_targets = config.n_negotiating_groups * config.n_targets_per_ng;
    derived.total_write_mbs =
        divup(derived.n_tracked_puts * config.chunk_size,1024L*1024L);
    derived.disk_kb_write_time =
        (unsigned)((TICKS_PER_SECOND/1000L)/config.mbs_per_target_drive);
    chunk_udp_packets = divup(config.chunk_size,UDP_SIZE_BYTES);
    derived.chunk_udp_xmit_duration =
        (config.chunk_size+MINIMUM_UDPV6_BYTES*chunk_udp_packets)*8L;
    chunk_tcp_packets = divup(config.chunk_size,TCP_SIZE_BYTES);
    derived.chunk_tcp_xmit_duration =
        (config.chunk_size+MINIMUM_TCPV6_BYTES*chunk_tcp_packets)*8L;
    
    derived.chunk_disk_write_duration =
        divup(config.chunk_size,1024)*derived.disk_kb_write_time;
   
    if (!config.gateway_mbs)
        derived.gateway_xmit_charge = 0;
    else {
        gateway_xmit_time =
            (unsigned)((TICKS_PER_SECOND/1000L)/config.gateway_mbs);
        derived.gateway_xmit_charge =
            divup(config.chunk_size,1024)*gateway_xmit_time*config.n_replicas;
    }
}

static char open_prefix[30];

static FILE *open_outf (const char *type)
{
    auto char name[66];
    
    assert(strlen(type) <= 20);

    if (open_prefix[0])
        sprintf(name,"%s.%s.csv",open_prefix,type);
    else
        sprintf(name,"%s.csv",type);
    return fopen(name,"w");
}

static void usage (const char *progname) {
    fprintf(stderr,"Usage: %s",progname);
    fprintf(stderr," [rep] [ch] [oh]");
    fprintf(stderr," [numrep <#>");
    fprintf(stderr," [duration <msecs>]");
    fprintf(stderr," [ngs <#>]");
    fprintf(stderr," [targets_per <#>]");
    fprintf(stderr," [chunk_size <kbytes>]\n");
    fprintf(stderr," [gateways <#>],");
    fprintf(stderr," [mbs <#>");
    fprintf(stderr," [gateway_mbs <#>");
    fprintf(stderr," [bwn <#>]");
    fprintf(stderr," [sample <uSecs>");
    fprintf(stderr," [terse]");
    fprintf(stderr," penalty <ticks_per_chunk>");
    fprintf(stderr," [cluster_trip_time <ticks>");
    fprintf(stderr,"\nOr %s help\n",progname);
    fprintf(stderr,"    to print this.\n");
    fprintf(stderr,"\nPenalty is assessed per chunk for Replicast overhead.\n");
    fprintf(stderr,"bwn is bid window multiplier\n");
    fprintf(stderr,"\nrep does replicast.\n");
    fprintf(stderr,"ch does consistent hash.\n");
    fprintf(stderr,"oh does omniscient hash.\n");
    fprintf(stderr,"Default is to do all\n");
}

static void log_config (FILE *f)
{
    float duration_msecs;
    
    if (replicast_sim.do_me) fprintf (f,"simulating Replicast\n");
    if (chtcp_sim.do_me) fprintf (f,"simulating CH/TCP\n");
    if (omhtcp_sim.do_me) fprintf (f,"simulating Omniscient Hash\n");
    duration_msecs = (float)config.sim_duration * 1000 / TICKS_PER_SECOND;
    fprintf(f,"config.sim_duration:%lu (0x%lx) ticks = %.2f msecs\n",
            config.sim_duration, config.sim_duration,duration_msecs);
    fprintf(f,"config.cluster_trip_time:%d\n",config.cluster_trip_time);
    fprintf(f,"confg.chunk_size:%d\n",config.chunk_size);
    fprintf(f,"config.mbs_per_target_drive:%d\n",
            config.mbs_per_target_drive);
    fprintf(f,"derived.gateway_xmit_charge:%lu\n",derived.gateway_xmit_charge);
    fprintf(f,"config.n_negotiating_groups:%d\n",config.n_negotiating_groups);
    fprintf(f,"config.n_replicas:%d\n",config.n_replicas);
    fprintf(f,"config.n_targets_per_ng:%d\n",config.n_targets_per_ng);
    fprintf(f,"derived.n_targets:%d\n",derived.n_targets);
    fprintf(f,"derived.chunk_udp_xmit_duration:%ld\n",
            derived.chunk_udp_xmit_duration);
    fprintf(f,"derived.chunk_tcp_xmit_duration:%ld\n",
            derived.chunk_tcp_xmit_duration);
    fprintf(f,"derived.chunk_disk_write_duration:%ld\n",
            derived.chunk_disk_write_duration);
    fprintf(f,"config.n_gateways:%d\n",config.n_gateways);
    fprintf(f,"config.penalty:%u\n",config.replicast_packet_processing_penalty);
    fprintf(f,"config.bid_window_multipler_pct:%u\n",
            config.bid_window_multiplier_pct);
    fprintf(f,"config.sample_interval:%d\n",config.sample_interval);
    fprintf(f,"config.seed:%d\n",config.seed);
    fprintf(f,"config.replicast_packet_processing_penalty:%d\n",
            config.replicast_packet_processing_penalty);
    if (config.terse) fprintf(f,"config.terse\n");
}

static protocol_t *protocol_match (const char *protocol_tag)
{
    return (0 == strcmp(protocol_tag,replicast_sim.tag)) ? &replicast_sim
    : (0 == strcmp(protocol_tag,repucast_sim.tag)) ? & repucast_sim
    : (0 == strcmp(protocol_tag,repgroup_sim.tag)) ? &repgroup_sim
    : (0 == strcmp(protocol_tag,chtcp_sim.tag)) ? &chtcp_sim
    : (0 == strcmp(protocol_tag,omhtcp_sim.tag)) ? &omhtcp_sim
    : (0 == strcmp(protocol_tag,omhudp_sim.tag)) ? &omhudp_sim
    : (protocol_t *)0;
}

static void customize_config (int argc, const char ** argv)
{
    const char *argv0 = argv[0];
    protocol_t *p;
    unsigned n_protocols = 0;
    
    if (argc == 2) {
        usage(argv0);
        exit(1);
    }
    for (--argc,++argv;argc >= 1;argv+=2,argc-=2) {
        if (0 == strcmp(*argv,"ngs"))
            config.n_negotiating_groups = atoi(argv[1]);
        else if (0 == strcmp(*argv,"numrep"))
            config.n_replicas = atoi(argv[1]);
        else if (0 == strcmp(*argv,"targets_per"))
            config.n_targets_per_ng = atoi(argv[1]);
        else if (0 == strcmp(*argv,"chunk_size"))
            config.chunk_size = atoi(argv[1])*1024;
        else if (0 == strcmp(*argv,"gateways"))
            config.n_gateways = atoi(argv[1]);
        else if (0 == strcmp(*argv,"duration"))
            config.sim_duration = atoi(argv[1]) * (TICKS_PER_SECOND/1000);
        else if (0 == strcmp(*argv,"seed"))
            config.seed = atoi(argv[1]);
        else if (0 == strcmp(*argv,"mbs"))
            config.mbs_per_target_drive = atoi(argv[1]);
        else if (0 == strcmp(*argv,"gateway_mbs"))
            config.gateway_mbs = atoi(argv[1]);
        else if (0 == strcmp(*argv,"cluster_trip_time"))
            config.cluster_trip_time = atoi(argv[1]);
        else if (0 == strcmp(*argv,"bwm"))
            config.bid_window_multiplier_pct = atoi(argv[1]);
        else if (0 == strcmp(*argv,"variance"))
            config.write_variance = atoi(argv[1]);
        else if (0 == strcmp(*argv,"sample"))
            config.sample_interval =
                atoi(argv[1])*(TICKS_PER_SECOND/(1000*1000));
        else if (0 == strcmp(*argv,"penalty"))
            config.replicast_packet_processing_penalty = atoi(argv[1]);
        else if (0 == strcmp(*argv,"terse")) {
            config.terse = true;
            --argv,++argc;
        }
        else if (0 == strcmp(*argv,"outprefix"))
            strncpy(open_prefix,argv[1],(sizeof open_prefix)-1);
        else if ((p = protocol_match(*argv)) != NULL) {
            p->do_me = true;
            ++n_protocols;
            --argv,++argc;
        }
        else {
            fprintf(stderr,"ERROR: bad arg %s %s\n\n",argv[0],argv[1]);
            usage(argv0);
            exit(1);
        }
    }
    if (!n_protocols)
        replicast_sim.do_me = chtcp_sim.do_me = true;
}

static void sim_protocol (const protocol_t *p)
{
    if (p->do_me) {
        printf     ("\nSimulating %s ******************************\n",p->name);
        fprintf(log_f,"Simulating %s ******************************\n",p->name);
        fprintf(bid_f,"Simulating %s ******************************\n",p->name);
        
        init_seqnum();
        simulate(p);
        free(track.durations);
        fprintf(log_f,"\n");
        fprintf(bid_f,"\n");
        fprintf(inflight_f,"\n");
    }
}

int main(int argc, const char * argv[]) {
    customize_config(argc,argv);
    derive_config();
    log_f = open_outf("log");
    bid_f = open_outf("bid");
    inflight_f = open_outf("inflight");
    
    log_config(log_f);
    
    sim_protocol(&replicast_sim);
    sim_protocol(&repucast_sim);
    sim_protocol(&repgroup_sim);
    sim_protocol(&chtcp_sim);
    sim_protocol(&omhtcp_sim);
    sim_protocol(&omhudp_sim);

    fclose(log_f);
    fclose(bid_f);
    fclose(inflight_f);

    exit(0);
}
