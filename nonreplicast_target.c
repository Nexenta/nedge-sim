//
//  nonreplicast_target.c
//  StorageClusterSim
//
//  Created by cait on 5/26/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#include "storage_cluster_sim.h"

typedef struct nonrep_target_t {
    unsigned concurrent_tcp_receives;
    tick_t last_disk_write_completion;
    unsigned mbz;
} nonrep_target_t;
//
// A struct target represents the target specific data that each individual
// target would have stored separately. This includes the queue of inbound
// reservations and when the last disk write completion would have occurred.
//

static nonrep_target_t *nrt;

void init_nonrep_targets(unsigned n_targets)
//
// Initialize the target subsystem, specifically the irhead for each
// entry in the target_t array 't' must be set to empty.
// Setting the ir_head to point to itself makes an empty list.
//
{ 
    nrt = (nonrep_target_t *)calloc(n_targets,sizeof(nonrep_target_t));
    assert(nrt);
}

void release_nonrep_targets (void)
{
    free(nrt);
    nrt = (nonrep_target_t *)0;
}

void handle_tcp_xmit_received (const event_t *e)

{
    assert(false);  // TODO: fixme
}

void handle_tcp_reception_complete (const event_t *e)
{
    assert(false);  // TODO: fixme
}
