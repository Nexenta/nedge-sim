//
//  config.h
//  StorageClusterSim
//
//  Created by cait on 5/21/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#ifndef StorageClusterSim_config_h
#define StorageClusterSim_config_h

// The raw inputs - change these to reconfigure the simulation


typedef struct sim_config {
    unsigned n_negotiating_groups;      // # of negotiating groups
    unsigned n_replicas;                // # of replicas per chunk, usually 3
    unsigned n_targets_per_ng;          // How many targets in each Negotiating
                                        // Group. This should be 2-4x n_replicas
    unsigned mbs_sec_per_target_drive;  // How fast do the target drives write?
                                        // Fast SSDs circa 2015 are 500 MB/sec.
                                        // Note that if this is faster than
                                        // 10 Gb/sec then the network will be
                                        // the bottleneck.

    unsigned chunk_size;            // size of each chunk.
                                    // Actual size is variable.
                                    // Recommend simulation values: 4K,64K,1M
    unsigned chunks_per_object;     // # of chunks per object
    unsigned tracked_object_puts;   // # of tracked object puts to perform
                                    // simulation concludes when these puts
                                    // have completed. Untracked object puts
                                    // will still be generated at same rate
                                    // to keep the pipeline running uniformly
    unsigned cluster_utilization;   // Object creation rate will be set to
                                    // make aggregate disk writes equal to
                                    // this percent of the raw disk write
                                    // speed potential. Expect spikes in latency
                                    // for consistent hashing when this exceeds
                                    // 50%
    unsigned seed;                  // seeds random # generators
} sim_config_t;

extern sim_config_t config;

// defaults
#define N_NEGOTIATING_GROUPS 256
#define N_TARGETS_PER_NG 6
#define MBS_SEC_PER_TARGET_DRIVE 500
#define N_REPLICAS 3
#define CHUNK_SIZE (128*1024)
#define CHUNKS_PER_OBJECT 10
#define TRACKED_OBJECT_PUTS 1000
#define CLUSTER_UTILIZATION 80

typedef struct sim_derived_config {
    unsigned n_targets;             // # of targets in the cluster.
    unsigned n_tracked_puts;        // # of tracked chunk puts to perform
    unsigned long total_write_mbs;
    unsigned disk_kb_write_time;
    tick_t chunk_xmit_duration;     // How long does it take to send a chunk?
    tick_t chunk_disk_write_duration;   // How long does it take to write it?
    tick_t ticks_per_object;        // average # of ticks between object puts
    unsigned objects_per_second;    // # of object puts per second
    unsigned objects_per_second_per_target; // per target
    unsigned mbs_per_second_per_target; 
} sim_derived_config_t;

extern sim_derived_config_t derived;

#define CLUSTER_TRIP_TIME       (2*10*1024)

#define NDEBUG

#endif
