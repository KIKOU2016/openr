/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/common/Flags.h"

DEFINE_int32(
    openr_ctrl_port,
    openr::Constants::kOpenrCtrlPort,
    "Port for the OpenR ctrl thrift service");
DEFINE_int32(
    kvstore_pub_port,
    openr::Constants::kKvStorePubPort,
    "KvStore publisher port for emitting realtime key-value deltas");
DEFINE_int32(
    kvstore_rep_port,
    openr::Constants::kKvStoreRepPort,
    "The port KvStore replier listens on");
DEFINE_int32(
    decision_pub_port,
    openr::Constants::kDecisionPubPort,
    "Decision publisher port for emitting realtime route-db updates");
// Flag to enable or disable plugin module.
DEFINE_bool(enable_plugin, false, "Enable plugin module");
DEFINE_int32(
    link_monitor_pub_port,
    openr::Constants::kLinkMonitorPubPort,
    "The port link monitor publishes on");
DEFINE_int32(
    link_monitor_cmd_port,
    openr::Constants::kLinkMonitorCmdPort,
    "The port link monitor listens for commands on");
DEFINE_int32(
    monitor_pub_port,
    openr::Constants::kMonitorPubPort,
    "The port monitor publishes on");
DEFINE_int32(
    monitor_rep_port,
    openr::Constants::kMonitorRepPort,
    "The port monitor replies on");
DEFINE_int32(
    fib_rep_port,
    openr::Constants::kFibRepPort,
    "The port fib replier listens on");
DEFINE_int32(
    health_checker_port,
    openr::Constants::kHealthCheckerPort,
    "The port health checker sends and recvs udp pings on");
DEFINE_int32(
    system_agent_port,
    openr::Constants::kSystemAgentPort,
    "Switch agent thrift service port for Platform programming.");
DEFINE_int32(
    fib_handler_port,
    openr::Constants::kFibAgentPort, // NOTE 100 is on purpose
    "Switch agent thrift service port for FIB programming.");
DEFINE_int32(
    spark_mcast_port,
    openr::Constants::kSparkMcastPort,
    "Spark UDP multicast port for sending spark-hello messages.");
DEFINE_string(
    platform_pub_url,
    "ipc:///tmp/platform-pub-url",
    "Publisher URL for interface/address notifications");
DEFINE_string(
    domain,
    "terragraph",
    "Domain name associated with this OpenR. No adjacencies will be formed "
    "to OpenR of other domains.");
DEFINE_string(
    chdir, "/tmp", "Change current directory to this after loading config");
DEFINE_string(listen_addr, "*", "The IP address to bind to");
DEFINE_string(
    config_store_filepath,
    "/tmp/aq_persistent_config_store.bin",
    "File name where to persist OpenR's internal state across restarts");
DEFINE_bool(
    assume_drained,
    false,
    "If set, will assume node is drained if no drain state is found in the "
    "persistent store");
DEFINE_string(
    node_name,
    "node1",
    "The name of current node (also serves as originator id");
DEFINE_bool(
    dryrun, true, "Run the process in dryrun mode. No FIB programming!");
DEFINE_string(loopback_iface, "lo", "The iface to configure with the prefix");
DEFINE_string(
    prefixes,
    "",
    "The prefix and loopback IP separated by comma for this node");
DEFINE_string(
    seed_prefix,
    "",
    "The seed prefix all subprefixes are to be allocated from. If empty, "
    "it will be injected later together with allocated prefix length");
DEFINE_bool(enable_prefix_alloc, false, "Enable automatic prefix allocation");
DEFINE_int32(alloc_prefix_len, 128, "Allocated prefix length");
DEFINE_bool(static_prefix_alloc, false, "Perform static prefix allocation");
DEFINE_bool(per_prefix_keys, false, "Create per IP prefix keys in Kvstore");
DEFINE_bool(
    set_loopback_address,
    false,
    "Set the IP addresses from supplied prefix param to loopback (/128)");
DEFINE_bool(
    override_loopback_addr,
    false,
    "If enabled then all global addresses assigned on loopback will be flushed "
    "whenever OpenR elects new prefix for node. Only effective when prefix "
    "allocator is turned on and set_loopback_address is also turned on");
DEFINE_string(
    ifname_prefix,
    "terra,nic1,nic2",
    "A comma separated list of strings. Linux interface names with a prefix "
    "matching at least one will be used for neighbor discovery, provided the "
    "interface is not excluded by the flag iface_regex_exclude");
DEFINE_string(
    iface_regex_include,
    "",
    "A comma separated list of extended POSIX regular expressions. Linux "
    "interface names containing a match (case insensitive) to at least one of "
    "these and not excluded by the flag iface_regex_exclude will be used for "
    "neighbor discovery");
DEFINE_string(
    iface_regex_exclude,
    "",
    "A comma separated list of extended POSIX regular expressions. Linux "
    "interface names containing a match (case insensitive) to at least one of "
    "these will not be used for neighbor discovery");
DEFINE_string(
    redistribute_ifaces,
    "",
    "The interface names or regex who's prefixes we want to advertise");
DEFINE_string(
    cert_file_path,
    "/tmp/cert_node_1.json",
    "my certificate file containing private & public key pair");
DEFINE_bool(enable_encryption, false, "Encrypt traffic between AQ instances");
DEFINE_bool(
    enable_rtt_metric,
    true,
    "Use dynamically learned RTT for interface metric values.");
DEFINE_bool(
    enable_v4,
    false,
    "Enable v4 in OpenR for exchanging and programming v4 routes. Works only "
    "when Switch FIB Agent is used for FIB programming. No NSS/Linux.");
DEFINE_bool(
    enable_subnet_validation,
    true,
    "Enable subnet validation on adjacencies to avoid mis-cabling of v4 "
    "address on different subnets on each end.");
DEFINE_bool(
    enable_lfa, false, "Enable LFA computation for quick reroute per RFC 5286");
DEFINE_bool(
    enable_ordered_fib_programming,
    false,
    "Enable ordered fib programming per RFC 6976");
DEFINE_bool(
    enable_bgp_route_programming,
    true,
    "Enable programming routes with prefix type BGP to the system FIB");
DEFINE_bool(enable_spark, true, "If set, enables Spark for neighbor discovery");
DEFINE_int32(
    decision_graceful_restart_window_s,
    -1,
    "Duration (in seconds) to wait for convergence upon restart before "
    "calculating new routes. Set to negative value to disable.");
DEFINE_int32(
    spark_hold_time_s,
    18,
    "How long (in seconds) to keep neighbor adjacency without receiving any "
    "hello packets.");
DEFINE_int32(
    spark_keepalive_time_s,
    2,
    "Keep-alive message interval (in seconds) for spark hello message "
    "exchanges. At most 2 hello message exchanges are required for graceful "
    "restart.");
DEFINE_int32(
    spark_fastinit_keepalive_time_ms,
    100,
    "Fast initial keep alive time (in milliseconds)");
DEFINE_string(
    spark_report_url, "inproc://spark_server_report", "Spark Report URL");
DEFINE_string(spark_cmd_url, "inproc://spark_server_cmd", "Spark Cmd URL");
DEFINE_int32(
    health_checker_ping_interval_s,
    10,
    "Time interval (in seconds) to send health check pings to other nodes in "
    "the network.");
DEFINE_bool(
    enable_health_checker,
    false,
    "If set, will send pings to other nodes in network at interval specified "
    "by health_checker_ping_interval flag");
DEFINE_bool(enable_fib_sync, false, "Enable periodic syncFib to FibAgent");
DEFINE_int32(
    health_check_option,
    static_cast<uint32_t>(
        openr::thrift::HealthCheckOption::PingNeighborOfNeighbor),
    "Health check scenarios, default set as ping neighbor of neighbor");
DEFINE_int32(
    health_check_pct, 0, "Health check pct % of nodes in entire topology");
DEFINE_bool(
    enable_netlink_fib_handler,
    false,
    "If set, netlink fib handler will be started for route programming.");
DEFINE_bool(
    enable_netlink_system_handler,
    true,
    "If set, netlink system handler will be started");
DEFINE_int32(
    ip_tos,
    openr::Constants::kIpTos,
    "Mark control plane traffic with specified IP-TOS value. Set this to 0 "
    "if you don't want to mark packets.");
DEFINE_int32(
    zmq_context_threads,
    1,
    "Number of ZMQ Context thread to use for IO processing.");
DEFINE_int32(
    link_flap_initial_backoff_ms,
    1000,
    "Initial backoff to dampen link flaps (in milliseconds)");
DEFINE_int32(
    link_flap_max_backoff_ms,
    60000,
    "Max backoff to dampen link flaps (in millseconds)");
DEFINE_bool(
    enable_perf_measurement,
    true,
    "Enable performance measurement in network.");
DEFINE_int32(
    decision_debounce_min_ms,
    10,
    "Fast reaction time to update decision spf upon receiving adj db update "
    "(in milliseconds)");
DEFINE_int32(
    decision_debounce_max_ms,
    250,
    "Decision debounce time to update spf in frequent adj db update "
    "(in milliseconds)");
DEFINE_bool(
    enable_watchdog,
    true,
    "Enable watchdog thread to periodically check aliveness counters from each "
    "openr thread, if unhealthy thread is detected, force crash openr");
DEFINE_int32(watchdog_interval_s, 20, "Watchdog thread healthcheck interval");
DEFINE_int32(watchdog_threshold_s, 300, "Watchdog thread aliveness threshold");
DEFINE_bool(
    enable_segment_routing, false, "Flag to disable/enable segment routing");
DEFINE_bool(set_leaf_node, false, "Flag to enable/disable node as a leaf node");
DEFINE_string(
    key_prefix_filters,
    "",
    "Only keys matching any of the prefixes in the list "
    "will be added to kvstore");
DEFINE_string(
    key_originator_id_filters,
    "",
    "Only keys with originator ID matching any of the originator ID will "
    "be added to kvstore.");
DEFINE_int32(memory_limit_mb, 300, "Memory limit in MB");
DEFINE_int32(
    kvstore_zmq_hwm,
    openr::Constants::kHighWaterMark,
    "Max number of packets to hold in kvstore ZMQ socket queue per peer");
DEFINE_int32(
    kvstore_flood_msg_per_sec,
    0,
    "Rate of Kvstore flooding in number of messages per second");
DEFINE_int32(
    kvstore_flood_msg_burst_size,
    0,
    "Burst size of Kvstore flooding in number of messages");
DEFINE_int32(
    kvstore_key_ttl_ms,
    openr::Constants::kKvStoreDbTtl.count(), // 5 min
    "TTL of a key (in ms) in the Kvstore");
DEFINE_int32(
    kvstore_sync_interval_s,
    openr::Constants::kStoreSyncInterval.count(),
    "Kvstore periodic random node sync interval in seconds");
DEFINE_int32(
    kvstore_ttl_decrement_ms,
    openr::Constants::kTtlDecrement.count(),
    "Amount of time to decrement TTL when flooding updates");
DEFINE_bool(
    enable_secure_thrift_server,
    false,
    "Flag to enable TLS for our thrift server");
DEFINE_string(
    x509_cert_path,
    "",
    "If we are running an SSL thrift server, this option specifies the "
    "certificate path for the associated wangle::SSLContextConfig");
DEFINE_string(
    x509_key_path,
    "",
    "If we are running an SSL thrift server, this option specifies the "
    "key path for the associated wangle::SSLContextConfig. If unspecified, "
    "will use x509_cert_path");
DEFINE_string(
    x509_ca_path,
    "",
    "If we are running an SSL thrift server, this option specifies the "
    "certificate authority path for verifying peers");
DEFINE_string(
    tls_ticket_seed_path,
    "",
    "If we are running an SSL thrift server, this option specifies the "
    "TLS ticket seed file path to use for client session resumption");
DEFINE_string(
    tls_ecc_curve_name,
    "prime256v1",
    "If we are running an SSL thrift server, this option specifies the "
    "eccCurveName for the associated wangle::SSLContextConfig");
DEFINE_string(
    tls_acceptable_peers,
    "",
    "A comma separated list of strings. Strings are x509 common names to "
    "accept SSL connections from. If an empty string is provided, the server "
    "will accept connections from any authenticated peer.");
DEFINE_int32(
    persistent_store_initial_backoff_ms,
    openr::Constants::kPersistentStoreInitialBackoff.count(),
    "Initial backoff to save DB to file (in milliseconds)");
DEFINE_int32(
    persistent_store_max_backoff_ms,
    openr::Constants::kPersistentStoreMaxBackoff.count(),
    "Max backoff to save DB to file (in millseconds)");
DEFINE_bool(enable_flood_optimization, false, "Enable flooding optimization");
DEFINE_bool(is_flood_root, false, "set myself as flooding root or not");
// TODO this option will be deprecated in near future, this is just for safely
// rollout purpose
DEFINE_bool(
    use_flood_optimization,
    false,
    "Enable this to use formed flooding topology to flood updates");
DEFINE_bool(
    prefix_fwd_type_mpls,
    false,
    "Advertise prefix forwarding type as SR MPLS to use label forwarding");
DEFINE_bool(
    prefix_algo_type_ksp2_ed_ecmp,
    false,
    "Advertise prefix algorithm type as 2-Shortest paths Edge Disjoint ECMP");
