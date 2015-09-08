//
//  StorageSim.h
//  CongestionSim
//
//  Created by cait on 9/3/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#ifndef CongestionSim_StorageSim_h
#define CongestionSim_StorageSim_h

#define divup(a,b) (((a)+((b)-1))/(b))

typedef unsigned long tick_t;
#define TICKS_PER_SECOND (10L*1000*1000*1000)

typedef struct node node_t;

typedef struct tllist { /* a circular timed link list sorted by 'time' */
    struct tllist *next,*prev;
    tick_t  time;
} tllist_t;

extern tick_t now;

typedef struct inbound_reservation {
    tllist_t    tllist;
    tick_t   lim;
     // reservation is for this message
    bool    accepted;
} inbound_reservation_t;




#endif