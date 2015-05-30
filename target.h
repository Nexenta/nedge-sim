//
//  target.h
//  StorageClusterSim
//
//  Created by cait on 5/16/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#ifndef StorageClusterSim_target_h
#define StorageClusterSim_target_h


typedef struct target {
    inbound_reservation_t ir_head;
    unsigned ir_queue_depth;
    tick_t last_disk_write_completion;
    unsigned mbz;
} target_t;
//
// A struct target represents the target specific data that each individual
// target would have stored separately. This includes the queue of inbound
// reservations and when the last disk write completion would have occurred.
//
extern target_t *t;

#endif
