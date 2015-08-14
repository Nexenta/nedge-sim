//
//  chtcp_gateway.c
//  StorageClusterSim
//
//  Created by cait on 8/10/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#include "storage_cluster_sim.h"

static void select_chtcp_targets (chunkput_t *c)

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
        inc_target_total_queue(t);
        fprintf(log_f,",%d",t);
    }
    fprintf(log_f,"\n");
}

#define MINIMUM_TCPV6_BYTES 74
#define TCP_CHUNK_SETUP_BYTES (3*MINIMUM_TCPV6_BYTES+200)
// 3 packets for TCP connectino setup plus minimal pre-transfer data
// The cluster_trip_time must still be added to this.

static void next_tcp_replica_xmit (chunkput_t *cp,tick_t time_now)

// Schedule the next TCP transmit start after the previous tcp transmit for
// the same object has completed

{
    tcp_xmit_received_t txr;
    unsigned r;
    
    if (cp->replicas_unacked) {
        txr.event.create_time = time_now;
        txr.event.tllist.time = time_now + config.cluster_trip_time*3 +
        TCP_CHUNK_SETUP_BYTES*8;
        txr.event.type = CHTCP_XMIT_RECEIVED;
        txr.cp = (chunk_put_handle_t)cp;
        r = cp->u.nonrep.repnum++;
        txr.target_num = cp->u.nonrep.ch_targets[r];
        insert_event(txr);
    }
}

void handle_chtcp_chunk_put_ready (const event_t *e)
{
    const chunk_put_ready_t *cpr = (const chunk_put_ready_t *)e;
    chunkput_t *cp = (chunkput_t *)cpr->cp;
    
    assert (cp);
    assert(!cp->mbz);
    
    select_chtcp_targets(cp);
    next_tcp_replica_xmit(cp,e->tllist.time);
}

void log_chtcp_chunk_put_ready (FILE *f,const event_t *e)
{
    fprintf(f,"chtcp,CHUNK_PUT_READY\n");
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

void handle_chtcp_reception_ack (const event_t *e)

// Handle the TCP reception ack, which occurs before the REPLICA_ACK
// The REPLICA_ACK occurs after the chunk replica is written.
// The tcp ack is sent once the chunk has been received.

{
    const tcp_reception_ack_t *tra = (const tcp_reception_ack_t *)e;
    chunkput_t *cp = (chunkput_t *)tra->cp;
    chunkput_t *new_cp;
    tick_t next_tcp_time;
    
    remove_tcp_reception_target(cp,tra->target_num);
    
    if (tra->max_ongoing_rx > cp->u.nonrep.max_ongoing_rx)
        cp->u.nonrep.max_ongoing_rx  = tra->max_ongoing_rx;
    next_tcp_time = e->tllist.time;
    next_tcp_time -= 3*config.cluster_trip_time;
    if (next_tcp_time <= now) next_tcp_time = now+1;
    if (++cp->u.nonrep.acked < config.n_replicas)
        next_tcp_replica_xmit(cp,next_tcp_time);
    else if ((new_cp = next_cp(cp->gateway)) != NULL)
        insert_next_chunk_put_ready(new_cp,next_tcp_time);
}

void log_chtcp_reception_ack (FILE *f,const event_t *e)
{
    const tcp_reception_ack_t *tra = (const tcp_reception_ack_t *)e;
    
    assert(e);
    fprintf(f,"0x%lx,0x%lx,non TCP_RECEPTION_ACK,0x%lx,%d,tgt,%d\n",
            e->tllist.time,e->create_time,tra->cp,chunk_seq(tra->cp),
            tra->target_num);
}
