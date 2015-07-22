//
//  storage_cluster_sim.h
//  StorageClusterSim
//
//  Created by Caitlin Bestler on 7/9/15.
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
#define TICKS_PER_SECOND (10L*1000*1000*1000)

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
    CHUNK_PUT_READY,
    REP_CHUNK_PUT_REQUEST_RECEIVED,
    REP_CHUNK_PUT_RESPONSE_RECEIVED,
    REP_CHUNK_PUT_ACCEPT_RECEIVED,
    REP_RENDEZVOUS_XFER_RECEIVED,
    TCP_XMIT_RECEIVED,
    TCP_RECEPTION_COMPLETE,
    TCP_RECEPTION_ACK,
    DISK_WRITE_START,
    DISK_WRITE_COMPLETION,
    REPLICA_PUT_ACK,
    CHUNK_PUT_ACK,
    TRACK_SAMPLE,
    
    NUM_EVENT_TYPES // MUST be last enum
} event_type_t;

typedef struct event {
    tllist_t tllist;
    tick_t create_time;     /* when the event was created */
    event_type_t type;
    unsigned sig;   // Must be 0x1234
} event_t;


// Tracking data

#define MAX_QDEPTH 10
#define MAX_WRITE_QDEPTH 30


typedef struct trackers {
    tick_t min_duration;
    tick_t max_duration;
    tick_t total_duration;
    unsigned long n_initiated;
    unsigned long n_writes_initiated;
    unsigned long n_writes_completed;
    unsigned long n_completions;
    unsigned n_active_targets;    // How many targets are currently writing?
    unsigned long n_reservation_conflicts;
    unsigned long n_reservations;
    bool     drain; // true when in drain mode
    unsigned mbz;
} trackers_t;

extern trackers_t track;

/* The specific events - This is also a handy summary of the timeline
 */


typedef struct chunk_put_ready {
    event_t event;          // chunk_put_ready is an event
    chunk_put_handle_t  cp; // Handle of the chunk put
} chunk_put_ready_t;
//
// The next Chunk Put for an object is to be handled by the Gateway.
//
// This is produced on the GAteway while handling the object_put_ready event
// (for the initial chunk) the subsequent events are generated when the gateway
// has credit and so that the next transmission will not start before the prior
// chunk has been transmitted.
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
    tick_t  estimated_ack;  // When is the estimated ack for this transfer
                            // This estimates the write delay and duration
    int qdepth;        // depth of target's inbound reservation queue
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
// 'cp' to 'target_num'.
// It is generated by the gateway and consumed by the target.
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
} tcp_reception_complete_t;
//
// This event models signals the schedule completion of the earliest
// ongoing reception for 'target_num'. Note that if another tcp reception
// was added in the interim that reception might not be actually complete.
// In that case the amount actually transferred is calculated and a new
// event is scheduled.
//

typedef struct tcp_reception_ack {
    event_t event;          // tpc_reception_ack is an event
    chunk_put_handle_t  cp; // handle ofthe chunk put
    unsigned target_num;    // ack is from this target
    unsigned max_ongoing_rx;    // maximum n_ongoing_receptions for target
                                // over lifespan of this transmission.
} tcp_reception_ack_t;

typedef struct disk_write_start {
    event_t event;          // disk_write_start is an event
    chunk_put_handle_t  cp; // Handle of the chunk put
    unsigned target_num;    // which target is completing the write?
    tick_t expected_done;   // when is this write expected to complete?
    int write_qdepth;
    unsigned *qptr;         // pointer to counter to be incremented when this
    // event is executed.
} disk_write_start_t;
//
// A Disk Write to persistent storage has been initiated for 'cp'
// on 'target_num'. A chunk_put_ack can now be scheduled on the gateway
//
// a Disk_Write_Start is scheduled after full reception of the payload
// either immediately (if there is no disk write queue for the target)
// or after already queued writes will complete.
//
typedef struct disk_write_completion {
    event_t event;          // disk_write_completion is an event
    chunk_put_handle_t  cp; // Handle of the chunk put
    unsigned target_num;    // which target is completing the write?
    int write_qdepth;  // depth of write queue encountered on this target
    unsigned *qptr;         // pointer to counter to be decremented when this
                            // event is executed.
} disk_write_completion_t;
//
// A Disk Write to persistent storage has been completed for 'cp''
// on 'target_num'. A chunk_put_ack can now be scheduled on the gateway
//
// A Disk Write Completion is scheduled after a fixed delay based on chunk
// sizeformt the matching disk_write_start event.
//

typedef struct replica_put_ack {
    event_t event;          // replica_put_ack is an event
    chunk_put_handle_t  cp; // Handle of the chunk put
    unsigned target_num;    // replica was created on this target
    int write_qdepth;  // depth of write queue encountered on this target
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
    int write_qdepth;  // max depth of write queue encountered
                            // for this chunk
} chunk_put_ack_t;
//
// This event reports that all replicas for chunk 'cp' have been acked.
// It is generated by the gateway while processing 'repica_put_ack' events
// and is consumed by the gateway.

typedef struct track_sample {
    event_t event;
} track_sample_t;

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
    unsigned write_qdepth;      // pending writes on this target
    unsigned total_inflight;    // chunk replicas assigned to this target
                                // but not yet acked to the gateway
} target_t;

// Chunkput Tracking
//
// Information about a pending Chunk Put is accessible only on the Gateway
// So the struct is only defined in this file. An unsigned long holds the
// pointer for the target event handlers.
//
// Each chunkput_t is allocated by the gateway, and then freed when it is done.

typedef struct bid {
    tick_t  start;
    tick_t  lim;
    tick_t  estimated_ack;
    unsigned target_num;
    unsigned queue_depth;
} bid_t;

//
// A bid by 'target_num' to store a chunk within the time range 'start'..'lim'
//
#define MAX_TARGETS_PER_NG 20

typedef struct chunkput_replicast {
    // replicast-specific ortion of chunkput_t
    unsigned ng;     // Chunk has been assigned to this NG
    bid_t    bids[MAX_TARGETS_PER_NG];
    // collected bid response
    // once rendezvous transfer is scheduled it is stored in bids[0].
    unsigned nbids; // # of bids collected
    unsigned responses_uncollected; // Chunk Put responses still to be collected
} chunkput_replicast_t;

typedef struct chunkput_nonreplicast {
    // non-repicast specfici portion of chunkput_t
    unsigned ch_targets[MAX_REPLICAS]; // selected targets
    unsigned repnum;                    // # of replicas previously generated.
    unsigned acked;
    unsigned max_ongoing_rx;    // maximum n_ongoing_receptions for any target
} chunkput_nonreplicast_t;

typedef struct chunkput {       // track gateway-specific info about a chunkput
    unsigned sig;               // must be 0xABCD
    unsigned seqnum;             // sequence # (starting at 1) for all chunks
    // put as part of this simulation
    unsigned gateway;           // which gateway?
    tick_t   started;           // When processing of this chunk started
    tick_t   done;              // When processing of this chunk completed
    unsigned replicas_unacked;  // Number of replicas not yet acked
    int write_qdepth;      // Maximum write queue depth encountered for
    // this chunk put
    union chunkput_u {
        chunkput_replicast_t replicast;
        chunkput_nonreplicast_t nonrep;
    } u;
    unsigned mbz;               // must be zero
} chunkput_t;

// Gateway event handlers - in gateway.c
extern chunkput_t *next_cp (tick_t start_tikme);
extern void handle_chunk_put_ready (const event_t *e);
extern void handle_rep_chunk_put_response_received (const event_t *e);
extern void handle_tcp_reception_ack (const event_t *e);
extern void handle_replica_put_ack (const event_t *e);
extern void handle_chunk_put_ack (const event_t *e);
extern void report_duration_stats (void);
extern unsigned chunk_seq (chunk_put_handle_t cph); // utility to fetch seq #

extern void init_seqnum(void);

// Common Target event handlers - in common_target.c
extern void handle_disk_write_start (const event_t *e);
extern void handle_disk_write_completion (const event_t *e);
extern void inc_target_total_queue(unsigned target_num);
extern void dec_target_total_queue(unsigned target_num);

// Replicast-specific Target event handlers - in replicast_target.c

extern void handle_rep_chunk_put_request_received (const event_t *e);
extern void handle_rep_chunk_put_accept_received (const event_t *e);
extern void handle_rep_rendezvous_xfer_received (const event_t *e);
extern void init_rep_targets(unsigned n_targets);
extern void report_rep_chunk_distribution(void);
extern void release_rep_targets(void);
extern target_t *rep_target (unsigned target_num);

// Consistent Hash / TCP specific Target event handlers
extern void handle_tcp_xmit_received (const event_t *e);
extern void handle_tcp_reception_complete (const event_t *e);
extern void init_nonrep_targets(unsigned n_targets);
extern void report_nonrep_chunk_distribution(void);
extern void release_nonrep_targets(void);
extern target_t *nonrep_target (unsigned target_num);
#endif


