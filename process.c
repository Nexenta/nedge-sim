//
//  process.c
//  CongestionSim
//
//  Created by cait on 9/10/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "StorageSim.h"
#include "network_sim.h"

void process (const node_t *node,const packet_t *pkt)
{
    if (pkt->last_packet) {
        switch (pkt->message->message_type) {
            case PUT_REQUEST:
                // formulate bid
                // send PUT_RESPONSE message to source
                break;
            case PUT_RESPONSE:
                // collect responses
                // if not all collected then done
                // select rendezvous group
                // send PUT_ACCEPT message to negotiating troup
                // send RENDEZVOUS_TRANSFER message to rendezvous group at time
                // if there is credit schedule initiate event just before
                //      the schedule completion of the bulk transfer
                break;
            case PUT_ACCEPT:
                // adjust inbound reservation
                break;
            case PUT_TRANSFER:
                // complete inbound transfer
                // simuilate disk activity
                // send PUT_ACK when disk is estimated to complete
                break;
            case PUT_ACK:
                // restore credit for source
                // which might schedule an initiate event
                break;
        }
    }
    // if this is the last packet for this message
    //      switch (msg->type)
    
}
