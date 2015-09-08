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

typedef enum queue_type {
    CONGESTION_CONTROL,
    OUTPUT_UNSOLICITED,
    OUTPUT_SOLICITED,
    CORE,
    INPUT_UNSOLICITED,
    INPUT_SOLICITED,
    PROCESS
} queue_type_t;

typedef struct node node_t;

typedef struct placed_packet placed_packet_t;

typedef struct queue_class {
    queue_type_t class_type;
    void (*process)(node_t *,placed_packet_t *);
} queue_class_t;

extern queue_class_t congestion_control_class;
extern queue_class_t output_unsolicited_class;
extern queue_class_t output_solicited_class;
extern queue_class_t core_class;
extern queue_class_t input_unsolicited_class;
extern queue_class_t input_solicited_class;
extern queue_class_t process_class;

typedef struct queue {
    const node_t  *node;    // is owned by
    const queue_class_t *class;
    unsigned qdepth;
    // queue depth
    // queue depth delta
    tllist_t    packet_queue_head;  // of packet_placement_t
} queue_t;

#define MAX_DESTINATIONS 12
typedef struct destination {
    size_t  n_destinations;
    unsigned target[MAX_DESTINATIONS];
} destination_t;

typedef enum storage_message_type {
    PUT_REQUEST,
    PUT_RESPONSE,
    PUT_ACK
} storage_messge_type_t;

typedef unsigned message_type_t; // union of all '*_message_type_t's

typedef struct config {
    size_t  per_packet_overhead;
    tick_t  congestion_xmit_window;
} config_t;

extern config_t config;

typedef struct message {
    unsigned        source;
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
} message_t;

typedef struct packet {
    const message_t *message;
    unsigned packet_num;
    size_t packet_size;
} packet_t;

struct placed_packet {
    // linked list entry noting entry and exit
    // of a packet from a queue
    //
    // Each placed packet has 4 relevant times:
    //      when it begins enqueuing here (tllist.time)
    //      when it is ready for processing here (finish_arrival)
    //      when it can be sent to the next step (start_xmit)
    //      when transmission/orocessing is done (end_xmit)
    //          transmission rate is infinite intra-system, 10 Gb/sec on wire
    //
    tllist_t tllist;
    tick_t  finsih_arrival;
    tick_t  start_xmit;
    tick_t  end_xmit;
    const packet_t *packet;
};

struct node {
    // congestion control tracking data
    queue_t congestion_control_delay_filter;
    queue_t output_unsolicited;
    queue_t output_solicited;
    queue_t core;
    queue_t input_unsolicited;
    queue_t input_solicited;
    queue_t process;
};

typedef void (*process_msg_t)(node_t *node,message_t *msg);
    // invoked in each destination node when message is ready to process

extern process_msg_t process_msg_plugin;
    // application layer must set this plug-in.

extern node_t *nodes;   // indexed by unsigned node number

extern void enqueue_packet (tick_t when, const packet_t *,queue_t *);

extern void simulate_msg_send (node_t *node,message_t *msg);

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

typedef struct event {
    tllist_t    tllist; // list of event_ts
    queue_t *q;  // which queue has something at its head now?
} event_t;



#endif
