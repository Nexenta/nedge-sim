//
//  network_sim.h
//  CongestionSim
//
//  Created by Caitlin Bestler on 9/3/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//
//
// The Network Simulation package models a network as a series of
// congestion points that delivers packets to an enumerated list
// of destination nodes.
//
// Timing of packets through the congestion points is tracked before
// the packet is delivered for endpoint processing.

#ifndef CongestionSim_network_sim_h
#define CongestionSim_network_sim_h


typedef struct node node_t;

typedef struct placed_packet placed_packet_t;


typedef struct queue {
    const node_t    *node;    // is owned by
    bool            is_input;
    tllist_t        packet_queue_head;  // of packet_placement_t
} queue_t;

typedef struct input_queue {
    queue_t queue;  // output_queue is a queue
    size_t  packet_depth;
    size_t  byte_depth;
    // queue depth delta
} input_queue_t;

typedef struct output_queue {
    queue_t queue;  // input_queue is a queue
    bool    xmitting_now;
    tick_t xmit_done_at;    // if xmitting_now, when will xmit be done?
} output_queue_t;

#define MAX_DESTINATIONS 12
typedef struct destination {
    size_t  n_destinations;
    unsigned target[MAX_DESTINATIONS];
} destination_t;

typedef enum storage_message_type {
    PUT_REQUEST,
    PUT_RESPONSE,
    PUT_ACCEPT,
    PUT_TRANSFER,
    PUT_ACK
} storage_messge_type_t;

typedef unsigned message_type_t; // union of all '*_message_type_t's

typedef struct config {
    size_t  n_nodes;
    size_t  per_packet_overhead;
    tick_t  congestion_xmit_window;
    tick_t  cluster_trip_time;
    bool    dual_network_traffic_class;
} config_t;

extern config_t config;

typedef struct message {
    node_t          *source;
    unsigned        transaction_num;
    message_type_t  message_type;
    bool            solicited;
    size_t          message_size;
    size_t          max_packet_size;
    // packet size is max_packet_size + config.per_packet_overhead for all but end
    // for (message_left = message_size;
    //      message_left >= 0;
    //      message_left -= max_packet_size)
    //  each packet is sent at configured speed.
    //  All pacing is done *before* the first packet of a command
    //  unsolicited message is sent.
    
    destination_t   dest;
    tick_t          initiated_at;
    tick_t          completed_at;
    unsigned        ref_count;  // inc for each packet created reffing this.
                                // dec when packet is consumed
                                // release message when this reaches zero.
} message_t;

typedef struct packet {
    const message_t *message; // exception: ref_count altered via cast override
    unsigned        packet_num;
    size_t          packet_size;
    bool            last_packet;
    unsigned        ref_count;  // inc for each packet placement
                                // dec when packet_placement is released
                                // release packet when this reaches zero.
} packet_t;

struct placed_packet {
    // linked list entry noting entry and exit
    // of a packet from a queue
    tllist_t tllist;
    tick_t  finsih_arrival;
    const packet_t *packet; // exception: ref_count altered via cast override
};

struct node {
    // congestion control tracking data
    output_queue_t output;
    input_queue_t input;
};

extern node_t *nodes;   // indexed by unsigned node number

extern void enqueue_packet (tick_t when,
                            bool prioritized,
                            const packet_t *,
                            output_queue_t *);

extern void destination_enqueue (const packet_t *,input_queue_t *);

extern void simulate_msg_send (node_t *from_node,message_t *msg);

extern void process (const node_t *node,const packet_t *pkt);

// When a placed_packet reaches the head of a queue:
//      if next step is another a congestion_point (or are congestion points)
//          queue packet to those queues with calculated timings.
//          update congestion tracking for the congestion point
//              generate traffic_tracking data and/or congestion notifications
//              Each type of information is available to congestion_control
//                  delay_filter modules
//                      traffic tracking may be delayed <config> ticks.
//
//                      congestion notificatin dings may be delayed <config>
//                      ticks.
//      otherwise
//          invoke app level handler for packet
//
// The app layer generates messages to traverse the simulated network:
//      client nodes spontaneously generate transactions at pseudo-random
//      intervals that are submitted to the congestion control tracking data
//
// The Multicast simulation:
//      new chunk becomes a Chunk Put Request queued to congest_control queue
//      This ultimately sends a Chunk Put Request to all of the nodes in a
//          Negotiating Group
//      targets receiving a Chunk Put Request generates a Chunk_Put_Response
//          which is sent to the Source of the Chunk Put Request
//          An Inbound Reservation is created to match the bid.
//      nodes receive Chunk Put Responses collect all of the Repsonse and then;
//          Determine receiving set for the Rendezvus Transfer.
//          Send unsolicited message (Chunk Put Accept) to the negotiataing Group
//          Sends solicited message (Rendezvous Transfer) to the rendezvous group
//      nodes recieving a Chunk Put Accept
//          Trim or cancel the Inbound Reservation.
//      nodes receiving a Rendezvous Transfer
//          when entire message has been received send unsolicited message
//              (Put Ack) after simulated write delay.
//
// Unicast simulation
//      new chunk becomes a Chunk Put Request sent to the next destination
//      receiving node determines when its input queue is available
//      Chunk Put Response indicates when to transmit.
//      Rendezvous Transfer is sent from source to destination
//      Chunk Put Ack is sent from destination back to source
//          which may select next desintation (and repeat) or complete.

typedef enum event_type {
    INITIATE_PUT,       // initiate a new chunk put transaction on this node
    CONGESTION_DING,    // excess queue depth detected and fedback with
                        // configurable delay
    MSG_XFER_START,     // Start of the transfer of a message from a queue
                        // to a queue, with the 'from' queue being optional.
                        // When the destination is the 'process' queue this
                        // will result in appication processing of the msg.
                        //
                        // Buffer usage must be incremented for the destination
                        // queue at this point.
    MSG_XFER_DONE,      // completion of a transfer.
                        // at the source the buffer claims can be reduced.
                        // at the destination the packet can be scheduled
                        // for transmission if it is at the head of the queue
                        // if not at the head of the queue it's order in the
                        // queue can be fixed.
} event_type_t;

typedef struct event {
    tllist_t    tllist; // list of event_ts
    event_type_t event_type;
} event_t;

typedef struct initiate_event { // for initiate_put
    event_t event;  // a generation_event is an event
    node_t  *node;  // which node should the new Chunk Put originate on?
} initiate_event_t;

typedef struct ding_event { // for congestion ding
    node_t  *dinger;    // destination that was congested.
    // TBD: is there a severity of the ding?
} ding_event_t;

typedef struct msg_xfer_start_event { // for msg_xfer_start
    event_t event;  // a msg_xfer_start_event is an event
    packet_t *pkt;
    node_t  *from_node; // from from_node->output_q
    node_t  *to_node;   // to to_node->input_q
} msg_xfer_start_event_t;

typedef struct msg_xfer_done_event { // for msg_xfer_done
    event_t event;  // a msg_xfer_done_event is an event
    packet_t *pkt;
    node_t  *from_node; // from from_node->output_q
    node_t  *to_node;   // to to_node->input_q
} msg_xfer_done_event_t;





#endif
