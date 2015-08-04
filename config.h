//
//  config.h
//  StorageClusterSim
//
//  Created by Caitlin Bestler on 7/9/15.
//  Copyright (c) 2015 Nexenta Systems. All rights reserved.
//

#ifndef StorageClusterSim_config_h
#define StorageClusterSim_config_h

// The raw inputs - change these to reconfigure the simulation


typedef struct sim_config {
    unsigned cluster_trip_time;         // # of bit-times to send zero-length
                                        // ethernet frame end-to-end.
    unsigned replicast_packet_processing_penalty; // extra time to penalize
                                        // replicast for each packet processed.
    unsigned n_negotiating_groups;      // # of negotiating groups
    unsigned n_replicas;                // # of replicas per chunk, usually 3
    unsigned n_targets_per_ng;          // How many targets in each Negotiating
                                        // Group. This should be 2-4x n_replicas
    unsigned mbs_per_target_drive;  // How fast do the target drives write?
                                        // Fast SSDs circa 2015 are 500 MB/sec.
                                        // Note that if this is faster than
                                        // 10 Gb/sec then the network will be
                                        // the bottleneck.
    unsigned utilization;           // if non-zero, limit the gateways to
                                    // producing this % of the possible IOPs.
    unsigned chunk_size;            // size of each chunk.
                                    // Actual size is variable.
                                    // Recommend simulation values: 4K,64K,1M
    unsigned long sim_duration;     // Simulation duration in network bit-ticks.
    unsigned n_gateways;            // # of gateways producing chunks
    unsigned write_variance;        // writes are +/- (write/(variance/2))
                                    // variance  is  1/nth the whole duration
                                    // centered on 1.0
    bool do_replicast;              // Test replicast
    bool do_ch;                     // Test Consistent Hash 
    unsigned seed;                  // seeds random # generators
    unsigned bid_window_multiplier_pct; // multiplier (%) for offered bid window
                                    // width
    unsigned sample_interval;       // take a TRACK_SAMPLE every <this many>
                                    // useconds
    bool terse;                     // if true, omit many events from log_f
} sim_config_t;

extern sim_config_t config;

// defaults

#define CLUSTER_TRIP_TIME       10000    // 1 microsecond
#define N_NEGOTIATING_GROUPS 50
#define N_TARGETS_PER_NG 9
#define MBS_PER_TARGET_DRIVE 400
#define N_REPLICAS 3
#define CHUNK_SIZE (128*1024)
#define N_GATEWAYS 40
#define PENALTY 15000                 	// 1.5 ms or 1.5 times TRIP time - replicast
                                     	// chunk processing overhead above TCP connection
                                     	// establishment (overhead).
#define BID_WINDOW_MULTIPLIER_PCT 210	// multiplier (%) for offered bid to allow
                                     	// gateway to find overlapping windows.
				     	// default 210 yields (window * 2.1)
#define SAMPLE_INTERVAL (TICKS_PER_SECOND/1000)    // 1msec

typedef struct sim_derived_config {
    unsigned n_targets;             // # of targets in the cluster.
    unsigned n_tracked_puts;        // # of tracked chunk puts to perform
    unsigned long total_write_mbs;
    unsigned disk_kb_write_time;
    tick_t chunk_udp_xmit_duration;     // How long to UDP send a chunk?
    tick_t chunk_tcp_xmit_duration;     // How long to TCP send a chunk?
    tick_t chunk_disk_write_duration;   // How long to write a chunk to disk?
    float   credit_multiplier;      // xmit credits per tick for gateway pacing
} sim_derived_config_t;

extern sim_derived_config_t derived;



#endif
