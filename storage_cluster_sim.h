//
//  storage_cluster_sim.h
//  StorageClusterSim
//
//  Created by cait on 5/19/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#ifndef StorageClusterSim_storage_cluster_sim_h
#define StorageClusterSim_storage_cluster_sim_h

#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#define divup(a,b) (((a)+((b)-1))/(b))

typedef unsigned long tick_t;
#define TICKS_PER_SECOND (10L*1024*1024*1024)

#include "config.h"

typedef unsigned long chunk_put_handle_t;


// Per Target data

typedef struct tllist { /* a circular timed link list sorted by 'time' */
    struct tllist *next,*prev;
    tick_t  time;
} tllist_t;

typedef struct inbound_reservation {
    tllist_t    tllist;
    tick_t   lim;
    chunk_put_handle_t cp;  // reservation is for this chunk
    bool    accepted;
} inbound_reservation_t;

typedef enum event_type {
    NULL_EVENT,
    OBJECT_PUT_READY,
    REP_CHUNK_PUT_READY,
    REP_CHUNK_PUT_REQUEST_RECEIVED,
    REP_CHUNK_PUT_RESPONSE_RECEIVED,
    REP_CHUNK_PUT_ACCEPT_RECEIVED,
    REP_RENDEZVOUS_XFER_RECEIVED,
    TCP_XMIT_RECEIVED,
    TCP_RECEPTION_COMPLETE,
    TCP_RECEPTION_ACK,
    DISK_WRITE_COMPLETION,
    REPLICA_PUT_ACK,
    CHUNK_PUT_ACK,
    
    NUM_EVENT_TYPES // MUST be last enum
} event_type_t;

typedef struct event {
    tllist_t tllist;
    tick_t create_time;     /* when the event was created */
    event_type_t type;
    unsigned sig;   // Must be 0x1234
} event_t;

/* The specific events - This is also a handy summary of the timeline
 */

typedef struct object_put_ready {
    event_t  event;             // object_put_ready is an event
    unsigned n_chunks;          // # of chunks in this object (include metadata)
} object_put_ready_t;
//
// An object with 'n_chunks' chunks (including metadata chunks) are queued
// to be put, each with 'n_replicas'.
//
// This is produced by the main loop to reach a configured io bandwidth.
// It is consumed by an anonymous Gateway, which will start a chain of
// Chunk Put Ready events to simulate putting the successive chunks for
// the object.
//

typedef struct rep_chunk_put_ready {
    event_t event;          // chunk_put_ready is an event
    chunk_put_handle_t  cp; // Handle of the chunk put
} rep_chunk_put_ready_t;
//
// The next Chunk Put for an object is to be handled by the Gateway.
// chunk_size and _replicas are inherited from the object_put_ready event
//
// This is produced on the GAteway while handling the object_put_ready event
// (for the initial chunk) and then serialized as each Rendezvous Transfer is
// received so that the handling of the next chunk begins after the prior chunk
// has been transmitted (but not necessraily yet written to the target disk).
//

typedef struct rep_chunk_put_request_received {
    event_t event;          // chunk_put_request_received is an event
    chunk_put_handle_t  cp; // Handle of the chunk put
    unsigned target_num;    // Which target is receiving this chunk_put_request?
} rep_chunk_put_request_received_t;
//
// The chunk_put_request_received event is triggered for each target in the
// receiving negotiating group at the same time. Each target will then determine
// it's response to the multicast request. These responses will differ when the
// existing inbound_reservations for each target differ.
//
// The Chunk Put Request is the first packet exchanged in a Replicast
// negotiation. It is multicast from the Gateway to the targets in the
// Negotiating Group
//

typedef struct rep_chunk_put_response_received {
    event_t event;          // chunk_put_response_received is an event
    chunk_put_handle_t  cp; // Handle of the chunk put
    unsigned target_num;    // Target that generated this response
    tick_t  bid_start;      // The start of the bid window from this target
    tick_t  bid_lim;        // End of the bid window from this target
    unsigned qdepth;        // depth of target's inbound reservation queue
} rep_chunk_put_response_received_t;
//
// Each target in the Negotiationg Group that receives a chunk_put_request
// must generate a Chunk_Put_Response indicating when it could accept the
// chunk.
//
// In a real Replicast Cluster this response would also indicate:
//      * whether the Chunk is already stored. This simulation is ignoring
//          distributed deduplication because the same distributed deduplication
//          is applicable whether Replicast or Consistent Hashing is used.
//      * how full the target drive is, allowing the cluster to load-balance
//          storage capacity as well as IOPs.
//
// Each target schedules its response to be processed by the gateway.
//
// The Chunk Put Response isthe 2nd step in a Replicast negotiation.
// It is unicast from each member of the Negotiating Group back to the
// originating Gateway.
//

#define MAX_REPLICAS 9
typedef struct rep_chunk_put_accept {
    event_t event;          // chunk_put_accept is an event
    chunk_put_handle_t  cp; // Handle of the chunk put
    tick_t  window_start;   // The start of the accepted window
    tick_t  window_lim;     // The end of the accepted window
    unsigned target_num;    // Target_num of the target receiving this message
    unsigned accepted_target[MAX_REPLICAS]; // the accepted targets
} rep_chunk_put_accept_t;
//
// The Chunk Put Accept message is the 3rdc step in the Replicast Negotiation.
//
// it is sent by the originating Gateway to each target in the Negotiating Group
// to tell them what subset of the group has been selected for the Rendezvous
// Transfer, and what subset of offered reserved inbound window has been
// accepted.
//

typedef struct rep_rendezvous_xfer_received {
    event_t event;          // rep_rendezvous_transfer_receieved is an event
    chunk_put_handle_t  cp; // Handle of the chunk put
    unsigned target_num;    // The target that received this rendezvous transfer
} rep_rendezvous_xfer_received_t;
//
// The Rendezvous Transfer is received by each selected target for a specific
// chunk put. it occurs when the full chunk transfer would be compelte.
//
// It is scheduled at the same time as the chunk_put_accept for the subset
// of the negotiating group which was accepted.
//

typedef struct tcp_xmit_received {
    event_t event;          // tcp_xmit_received is an event
    chunk_put_handle_t  cp; // Handle of the chunk put
    unsigned target_num;    // Target receiving this chunk
} tcp_xmit_received_t;
//
// This event represents receipt ofthe first byte  of a TCP trasmission of chunk
// 'cp' to 'target_num'. It is generated by the gateway and consumed by the target.
//
// The target will then simulate reception of this and any other concurrently
// received TCP flows to this target to schedule TCP_RECEPTION_COMPLETE events
//
// This is a very generous model of TCP congestions, where we allow some future
// TCP congestion algorithm to *instantly* converge on evenly dividing the
// egress port capacity evenly over N payload flows.
//

typedef struct tcp_reception_complete {
    event_t     event;          // tcp_reception_complete is an event
    chunk_put_handle_t  cp;     // Handle of the chunk put
    unsigned    target_num;     // Target where this reception completed
    tick_t      credit;         // already received data
} tcp_reception_complete_t;
//
// This event models completion of one TCP transmission (of 'cp') to a
// specific target ('target_num')
//
// As a result the remaining competing flows will speed up (we generously
// model this as being instantaneous) which will result in scheduling one of
// the remaining flows completing.
//
// The disk write is also scheduled at this step, as descried for replicast.
//

typedef struct tcp_reception_ack {
    event_t event;          // tpc_reception_ack is an event
    chunk_put_handle_t  cp; // handle ofthe chunk put
    unsigned target_num;    // ack is from this target
} tcp_reception_ack_t;

typedef struct disk_write_completion {
    event_t event;          // disk_write_completion is an event
    chunk_put_handle_t  cp; // Handle of the chunk put
    unsigned target_num;    // which target is completing the write?
    unsigned write_qdepth;  // depth of write queue encountered on this target
    unsigned *qptr;         // pointer to counter to be decremented when this
                            // event is executed.
} disk_write_completion_t;
//
// A Disk Write to persistent storage has been completed for'chunk_num'
// on 'target_num'. A chunk_put_ack can now be scheduled on the gateway
//
// A Disk Write Completion is scheduled after full reception of the payload.
// The simulated disk write begins when the full reception of the chunk has
// completed, or after the latest completion of any other disk write for this
// target disk.
//
// Modeling disk_write_completions has an impact on overall throughput when
// a target disk is slower than the 10 GbE wire speed emulated.
//

typedef struct replica_put_ack {
    event_t event;          // replica_put_ack is an event
    chunk_put_handle_t  cp; // Handle of the chunk put
    unsigned target_num;    // replica was created on this target
    unsigned write_qdepth;  // depth of write queue encountered on this target
} replica_put_ack_t;
//
// This event reports to the gateway that one of the replicas for chunk
// 'cp' has been successfully written on 'target_num'.
//
// The gateway will determine when all the replicas have been acked, so that
// can consider the chunk to have been successfully written.
//

typedef struct chunk_put_ack {
    event_t event;          // chunk_put_ack is an event
    chunk_put_handle_t  cp; // Handle of the chunk put
    unsigned write_qdepth;  // max depth of write queue encountered
                            // for this chunk
} chunk_put_ack_t;
//
// This event reports that all replicas for chunk 'cp' have been acked.
// It is generated by the gateway while processing 'repica_put_ack' events
// and is consumed by the gateway.

//
// Event engine routines - main.c
//
#define insert_event(e) (_insert_event(&e.event,sizeof e))
extern void _insert_event (const event_t *new_event,size_t event_size);
    // allocate an event (event_size bytes) and copy 'new_event' to it
    // before invoking __insert_event()

extern void __insert_event (event_t *n); // insert already allocated event
                                         // into the event list (&ehead)
extern void tllist_remove (tllist_t *t); // remove 't' from tllist
                                         // 't' must still be eventually free()d
extern const tllist_t *tllist_find (
                                    const tllist_t *head,
                                    tick_t find);
void tllist_insert (tllist_t *ref,tllist_t *n);
#ifdef NDEBUG
#define tllist_node_verify(x)
#define tllist_verify(x)
#else
extern void tllist_node_verify (const tllist_t *t);
extern void tllist_verify (const tllist_t *h);
#endif
extern tick_t now;
extern bool replicast;
extern FILE *bid_f;
extern FILE *log_f;


typedef struct target { // common fields for replicast and non-replicast target
    unsigned write_qdepth;
} target_t;

    // are we simulating replicast or non-replicast right now?

// Gateway event handlers - in gateway.c
void handle_object_put_ready (const event_t *e);
extern void handle_chunk_put_ready (const event_t *e);
extern void handle_rep_chunk_put_response_received (const event_t *e);
extern void handle_tcp_reception_ack (const event_t *e);
extern void handle_replica_put_ack (const event_t *e);
extern bool handle_chunk_put_ack (const event_t *e);
extern void report_duration_stats (void);
extern unsigned chunk_seq (chunk_put_handle_t cp); // utility to fetch seq #

// Common Target event handlers - in common_target.c
extern void handle_disk_write_completion (const event_t *e);

// Replicast-specific Target event handlers - in replicast_target.c

extern void handle_rep_chunk_put_request_received (const event_t *e);
extern void handle_rep_chunk_put_accept_received (const event_t *e);
extern void handle_rep_rendezvous_xfer_received (const event_t *e);
extern void init_rep_targets(unsigned n_targets);
extern void release_rep_targets(void);

// Consistent Hash / TCP specific Target event handlers
extern void handle_tcp_xmit_received (const event_t *e);
extern void handle_tcp_reception_complete (const event_t *e);
extern void init_nonrep_targets(unsigned n_targets);
extern void release_nonrep_targets(void);
#endif


