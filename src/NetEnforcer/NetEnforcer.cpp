// NetEnforcer.cpp - Network traffic enforcement.
// NetEnforcer configures Linux Traffic Control (TC) at each host machine to enforce priorities and rate limits on network traffic.
// NetEnforcer is run on the machines hosting the VMs, and is configured through the NetEnforcer_prot RPC interface (see prot/net_prot.x).
//
// TC allows for a hierarchy of queueing disciplines (qdisc) and classes to manage network QoS.
// TC identifies qdiscs by a handle (e.g., [handle:]). TC identifies classes by a handle and minor number (e.g., [handle:minor]).
//
// NetEnforcer configures TC as follows:
// The root qdisc is a Hierarchical Token Bucket (HTB) with handle [1:].
// Within the root HTB qdisc, there is a tree structure of priority levels, starting with [1:rootHTBMinorHelper(0)].
// [1:rootHTBMinorHelper(0)] branches off into the class representing priority 0, [1:rootHTBMinor(0)], and the class representing the priorities higher than 0, [1:rootHTBMinorHelper(1)].
// [1:rootHTBMinorHelper(1)] branches off into the class representing priority 1, [1:rootHTBMinor(1)], and the class representing the priorities higher than 1, [1:rootHTBMinorHelper(2)].
// This sequence repeats until the last priority level, [1:rootHTBMinor(g_numPriorities - 1)], and the remaining best effort class, [1:rootHTBMinorDefault()].
//
// After this root HTB qdisc, there are DSMARK qdiscs attached to each priority level to tag the DSCP flags.
// For each priority level, there is a DSMARK qdisc with handle [DSMARKHandle(priority):] as a child of the priority level in the root HTB (i.e., [1:rootHTBMinor(priority)]).
// Each DSMARK qdisc, [DSMARKHandle(priority):], has one class [DSMARKHandle(priority):1], which performs the DSCP flag marking.
//
// The entire qdisc/class hierarchy is as follows:
//                           [1:]                                                                                                                              |
//                            |                                                                                                                                |
//                [1:rootHTBMinorHelper(0)]                                                                                                                    |
//                /                       \                                                                                                                    |
// [1:rootHTBMinor(0)]                 [1:rootHTBMinorHelper(1)]                                                                                               |
//          |                          /                       \                                                                                               |
// [DSMARKHandle(0):]   [1:rootHTBMinor(1)]                 [1:rootHTBMinorHelper(2)]                                                                          |
//          |                    |                          /                       \                                                                          |
// [DSMARKHandle(0):1]  [DSMARKHandle(1):]   [1:rootHTBMinor(2)]                 [1:rootHTBMinorHelper(3)]                                                     |
//                               |                    |                          /                       \                                                     |
//                      [DSMARKHandle(1):1]  [DSMARKHandle(2):]   [1:rootHTBMinor(3)]                 [1:rootHTBMinorHelper(4)]                                |
//                                                    |                    |                          /                       \                                |
//                                                   ...                  ...                        ...                     ...                               |
//                                                                                                              /                       \                      |
//                                                                       [1:rootHTBMinorHelper(g_numPriorities - 1)]                 [1:rootHTBMinorDefault()] |
//
// Lastly, as clients are added, src/dst filters are setup to send packets to the corresponding queue for its priority level.
//
// Copyright (c) 2017 Timothy Zhu.
// Licensed under the MIT License. See LICENSE file for details.
//

#include <iostream>
#include <cstdlib>
#include <cassert>
#include <string>
#include <map>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <arpa/inet.h>
#include <rpc/rpc.h>
#include <rpc/pmap_clnt.h>
#include "../prot/net_prot.h"
#include "../common/time.hpp"

#define MAX_CMD_SIZE 256

using namespace std;

struct Client {
    unsigned int id;
    unsigned int priority;
    unsigned int rateLimitLength;
    double rate;
    uint64_t lastSentBytesTime;
    double maxSentBytes;
    uint64_t prevSentBytes;
    uint64_t sentBytes;
};

// Globals
map<pair<unsigned long, unsigned long>, Client> g_clients;
unsigned int g_nextId = 0;
string g_dev = "eth0";
unsigned int g_maxRate = 125000000; // bytes per second
unsigned int g_numPriorities = 7;
unsigned int g_numLevels = 5;

// Handle for root HTB qdisc
unsigned int rootHTBHandle()
{
    return 1;
}

// Minor number within root HTB for class representing queue of a given priority level; starts at 1
unsigned int rootHTBMinor(unsigned int priority)
{
    return priority + 1;
}

// Minor number within root HTB for class helping to represent queue of a given priority level; starts after rootHTBMinor
unsigned int rootHTBMinorHelper(unsigned int priority)
{
    return priority + rootHTBMinor(g_numPriorities);
}

// Minor number within root HTB for default class; must start after rootHTBMinorHelper
unsigned int rootHTBMinorDefault()
{
    return rootHTBMinorHelper(g_numPriorities);
}

// Handle for DSMARK qdisc; starts after rootHTBMinorDefault to avoid confusion from reusing numbers
unsigned int DSMARKHandle(unsigned int priority)
{
    return priority + rootHTBMinorDefault() + 1;
}

// Handle for HTB rate limiters; starts after DSMARKHandle
unsigned int HTBBaseHandle(unsigned int priority)
{
    return priority + DSMARKHandle(g_numPriorities);
}

// Handle for HTB rate limiters; starts after HTBBaseHandle
unsigned int HTBHandle(unsigned int id, unsigned int priority, unsigned int level)
{
    unsigned int offset = (id * g_numPriorities * g_numLevels) + (priority * g_numLevels) + level;
    return offset + HTBBaseHandle(g_numPriorities);
}

// Minor number within HTB qdiscs
unsigned int HTBMinor(unsigned int id, unsigned int level)
{
    // Minor number 1 is reserved for default traffic
    return (level == 0) ? (id + 2) : 1;
}

// Execute a command and return the output as a string
string runCmd(char* cmd)
{
    FILE* pipe = popen(cmd, "r");
    if (pipe == NULL) {
        cerr << "Error running command: " << cmd << endl;
        return "";
    }
    char buf[128];
    string result = "";
    while (!feof(pipe)) {
        if (fgets(buf, 128, pipe) != NULL) {
            result += buf;
        }
    }
    pclose(pipe);
    return result;
}

// Remove the root qdisc in TC
void removeRoot()
{
    char cmd[MAX_CMD_SIZE];
    snprintf(cmd, MAX_CMD_SIZE,
             "tc qdisc del dev %s root",
             g_dev.c_str());
    runCmd(cmd);
}

// Remove a qdisc in TC
void removeQdisc(unsigned int parentHandle, unsigned int parentMinor, unsigned int childHandle)
{
    char cmd[MAX_CMD_SIZE];
    snprintf(cmd, MAX_CMD_SIZE,
             "tc qdisc del dev %s parent %d:%d handle %d:",
             g_dev.c_str(),
             parentHandle,
             parentMinor,
             childHandle);
    runCmd(cmd);
}

// Remove a class in TC
void removeClass(unsigned int parentHandle, unsigned int minor)
{
    char cmd[MAX_CMD_SIZE];
    snprintf(cmd, MAX_CMD_SIZE,
             "tc class del dev %s classid %d:%d",
             g_dev.c_str(),
             parentHandle,
             minor);
    runCmd(cmd);
}

// Remove a filter in TC from qdisc [parentHandle:] for a client with given id
void removeFilter(unsigned int parentHandle, unsigned int id)
{
    // We overload prio to be the client id + 1 to make the filter easy to identify when removing it.
    // Since only one filter should target a client, setting prio should not have any effect.
    char cmd[MAX_CMD_SIZE];
    snprintf(cmd, MAX_CMD_SIZE,
             "tc filter del dev %s parent %d: prio %d u32",
             g_dev.c_str(),
             parentHandle,
             id + 1);
    runCmd(cmd);
}

// Add a HTB qdisc in TC
void addHTBQdisc(unsigned int parentHandle, unsigned int parentMinor, unsigned int childHandle)
{
    char cmd[MAX_CMD_SIZE];
    snprintf(cmd, MAX_CMD_SIZE,
             "tc qdisc add dev %s parent %d:%d handle %d: htb default 1",
             g_dev.c_str(),
             parentHandle,
             parentMinor,
             childHandle);
    runCmd(cmd);
}

// Add a HTB class in TC
void addHTBClass(unsigned int parentHandle, unsigned int minor, unsigned int rate, unsigned int ceil, unsigned int burst, unsigned int cburst)
{
    char burstStr[MAX_CMD_SIZE] = "";
    if (burst > 0) {
        snprintf(burstStr, MAX_CMD_SIZE,
                 " burst %db",
                 burst);
    }
    char cburstStr[MAX_CMD_SIZE] = "";
    if (cburst > 0) {
        snprintf(cburstStr, MAX_CMD_SIZE,
                 " cburst %db",
                 cburst);
    }
    char cmd[MAX_CMD_SIZE];
    snprintf(cmd, MAX_CMD_SIZE,
             "tc class replace dev %s parent %d: classid %d:%d htb rate %dbps ceil %dbps%s%s",
             g_dev.c_str(),
             parentHandle,
             parentHandle,
             minor,
             rate,
             ceil,
             burstStr,
             cburstStr);
    runCmd(cmd);
}

// Add a filter in TC to qdisc [parentHandle:] for a client with given id
// Causes packets with given src/dst to use class [parentHandle:minor]
void addFilter(unsigned int parentHandle, unsigned int id, unsigned long s_dstAddr, unsigned long s_srcAddr, unsigned int minor)
{
    // Convert address to string
    char dstAddrStr[INET_ADDRSTRLEN];
    char srcAddrStr[INET_ADDRSTRLEN];
    struct in_addr dstAddr;
    struct in_addr srcAddr;
    dstAddr.s_addr = s_dstAddr;
    inet_ntop(AF_INET, &dstAddr, dstAddrStr, INET_ADDRSTRLEN);
    srcAddr.s_addr = s_srcAddr;
    inet_ntop(AF_INET, &srcAddr, srcAddrStr, INET_ADDRSTRLEN);

    // We overload prio to be the client id + 1 to make the filter easy to identify when removing it.
    // Since only one filter should target a client, setting prio should not have any effect.
    char cmd[MAX_CMD_SIZE];
    snprintf(cmd, MAX_CMD_SIZE,
             "tc filter add dev %s parent %d: protocol ip prio %d u32 match ip dst %s match ip src %s flowid %d:%d",
             g_dev.c_str(),
             parentHandle,
             id + 1,
             dstAddrStr,
             srcAddrStr,
             parentHandle,
             minor);
    runCmd(cmd);
}

// Initialize TC with our basic qdisc/class structure (see file header)
void initTC()
{
    // Remove root to start at a clean slate
    removeRoot();
    // Reserve 1% of bandwidth for each priority level, and assign remaining bandwidth to highest priority
    const unsigned int minRate = g_maxRate / 100; // bps
    unsigned int rate = minRate * (g_numPriorities + 1);
    unsigned int ceil = g_maxRate;
    char cmd[MAX_CMD_SIZE];
    // Create root HTB qdisc [1:]
    snprintf(cmd, MAX_CMD_SIZE,
             "tc qdisc add dev %s root handle 1: htb default %d",
             g_dev.c_str(),
             rootHTBMinorDefault());
    runCmd(cmd);
    // Create root HTB class [1:rootHTBMinorHelper(0)]
    snprintf(cmd, MAX_CMD_SIZE,
             "tc class add dev %s parent 1: classid 1:%d htb rate %dbps prio %d",
             g_dev.c_str(),
             rootHTBMinorHelper(0),
             g_maxRate,
             0);
    runCmd(cmd);
    for (unsigned int priority = 0; priority < g_numPriorities; priority++) {
        // Create root HTB class [1:rootHTBMinor(priority)]
        snprintf(cmd, MAX_CMD_SIZE,
                 "tc class add dev %s parent 1:%d classid 1:%d htb rate %dbps ceil %dbps prio %d",
                 g_dev.c_str(),
                 rootHTBMinorHelper(priority),
                 rootHTBMinor(priority),
                 minRate,
                 ceil,
                 priority);
        runCmd(cmd);
        // Add DSMARK qdisc [DSMARKHandle(priority):]
        snprintf(cmd, MAX_CMD_SIZE,
                 "tc qdisc add dev %s parent 1:%d handle %d: dsmark indices 2 default_index 1",
                 g_dev.c_str(),
                 rootHTBMinor(priority),
                 DSMARKHandle(priority));
        runCmd(cmd);
        // Set DSCP flag for DSMARK class [DSMARKHandle(priority):1]
        // Highest priority (0) is cs7 (0b11100000)
        unsigned char value = (7 - priority) << 5;
        snprintf(cmd, MAX_CMD_SIZE,
                 "tc class change dev %s classid %d:1 dsmark mask 0x3 value 0x%x", // must be change, not add
                 g_dev.c_str(),
                 DSMARKHandle(priority),
                 value);
        runCmd(cmd);
        // Create base HTB qdisc [HTBBaseHandle(priority):] for handling rate limits
        addHTBQdisc(DSMARKHandle(priority), 1, HTBBaseHandle(priority));
        // Create root HTB class [1:rootHTBMinorHelper(priority + 1)]
        rate -= minRate;
        ceil -= minRate;
        snprintf(cmd, MAX_CMD_SIZE,
                 "tc class add dev %s parent 1:%d classid 1:%d htb rate %dbps ceil %dbps prio %d",
                 g_dev.c_str(),
                 rootHTBMinorHelper(priority),
                 rootHTBMinorHelper(priority + 1),
                 rate,
                 ceil,
                 priority + 1);
        runCmd(cmd);
    }
}

// Get TC stats on the sent bytes
uint64_t getSentBytes(unsigned int parentHandle, unsigned int minor)
{
    char cmd[MAX_CMD_SIZE];
    snprintf(cmd, MAX_CMD_SIZE,
             "tc -s class show dev %s parent %d:",
             g_dev.c_str(),
             parentHandle);
    string stats = runCmd(cmd);
    char find[64];
    snprintf(find, 64,
             "class htb %d:%d",
             parentHandle,
             minor);
    size_t location = stats.find(find);
    if (location == string::npos) {
        return 0;
    }
    size_t begin = stats.find("Sent", location + 1);
    assert(location != string::npos);
    size_t end = stats.find('\n', begin);
    string statsLine = stats.substr(begin, end - begin);
    unsigned long long sentBytes;
    sscanf(statsLine.c_str(), "Sent %lld", &sentBytes);
    return sentBytes;
}

// Update sent bytes stats
void updateSentBytes(Client& c)
{
    if (c.rateLimitLength > 0) {
        uint64_t currSentBytes = getSentBytes(HTBBaseHandle(c.priority), HTBMinor(c.id, 0));
        c.sentBytes += currSentBytes - c.prevSentBytes;
        c.prevSentBytes = currSentBytes;
        uint64_t now = GetTime();
        c.maxSentBytes += c.rate * ConvertTimeToSeconds(now - c.lastSentBytesTime);
        c.lastSentBytesTime = now;
    }
}

// Update client with to use given priority level and rate limits
void updateClient(unsigned long s_dstAddr, unsigned long s_srcAddr, unsigned int priority, unsigned int rateLimitLength, double* rateLimitRates, double* rateLimitBursts)
{
    pair<unsigned long, unsigned long> addr(s_dstAddr, s_srcAddr);
    bool newClient = (g_clients.find(addr) == g_clients.end());
    if (newClient && (priority == g_numPriorities)) {
        return;
    }
    // Update client parameters
    Client& c = g_clients[addr];
    unsigned int oldPriority;
    unsigned int oldRateLimitLength;
    if (newClient) {
        c.id = g_nextId++;
        c.lastSentBytesTime = GetTime();
        c.maxSentBytes = 0;
        c.prevSentBytes = 0;
        c.sentBytes = 0;
        oldPriority = g_numPriorities;
        oldRateLimitLength = 0;
    } else {
        updateSentBytes(c);
        oldPriority = c.priority;
        oldRateLimitLength = c.rateLimitLength;
    }
    c.priority = priority;
    c.rateLimitLength = rateLimitLength;
    c.rate = (rateLimitLength > 0) ? rateLimitRates[0] : g_maxRate; // Occupancy calculation assumes just a single rate
    // Add/update HTB rate limiters
    unsigned int id = c.id;
    unsigned int level = 0;
    unsigned int parentHandle = HTBBaseHandle(priority);
    unsigned int minor = HTBMinor(id, level);
    unsigned int childHandle = HTBHandle(id, priority, level);
    while ((level * 2) < rateLimitLength) {
        if (level > 0) {
            // Add qdisc if necessary
            if (((level * 2) >= oldRateLimitLength) || (oldPriority != priority)) {
                addHTBQdisc(parentHandle, minor, childHandle);
            }
            // Change handles
            parentHandle = childHandle;
            minor = HTBMinor(id, level);
            childHandle = HTBHandle(id, priority, level);
        }
        // Get rates/bursts
        unsigned int rate = rateLimitRates[(level * 2)];
        unsigned int burst = rateLimitBursts[(level * 2)];
        unsigned int ceil;
        unsigned int cburst;
        if (((level * 2) + 1) < rateLimitLength) {
            ceil = rateLimitRates[(level * 2) + 1];
            cburst = rateLimitBursts[(level * 2) + 1];
        } else {
            ceil = rate;
            cburst = burst;
        }
        // Add/modify HTB class rates
        addHTBClass(parentHandle, minor, rate, ceil, burst, cburst);
        level++;
    }
    if ((rateLimitLength > 0) && ((oldRateLimitLength == 0) || (oldPriority != priority))) {
        // Add HTB filter if necessary
        addFilter(HTBBaseHandle(priority), id, s_dstAddr, s_srcAddr, HTBMinor(id, 0));
    }
    if (oldPriority != priority) {
        c.prevSentBytes = 0; // Reset prev sent bytes when switching to a new HTB class
        if (oldPriority < g_numPriorities) {
            // Remove old filter for priority level
            removeFilter(rootHTBHandle(), id);
        }
        if (priority < g_numPriorities) {
            // Add filter for priority level
            addFilter(rootHTBHandle(), id, s_dstAddr, s_srcAddr, rootHTBMinor(priority));
        }
    }
    if (oldRateLimitLength > 2) {
        if (oldPriority != priority) {
            // Remove old HTB chain
            removeQdisc(HTBBaseHandle(oldPriority), HTBMinor(id, 0), HTBHandle(id, oldPriority, 0));
        } else if ((level * 2) < oldRateLimitLength) {
            // Remove unnecessary qdiscs
            removeQdisc(parentHandle, minor, childHandle);
        }
    }
    if ((oldRateLimitLength > 0) && ((rateLimitLength == 0) || (oldPriority != priority))) {
        // Remove old HTB filter
        removeFilter(HTBBaseHandle(oldPriority), id);
        // Remove old HTB class
        removeClass(HTBBaseHandle(oldPriority), HTBMinor(id, 0));
    }
    if (priority == g_numPriorities) {
        // Remove from map
        g_clients.erase(addr);
    }
}

// Get occupancy of (dst/src) since last call
double getOccupancy(unsigned long s_dstAddr, unsigned long s_srcAddr)
{
    double occupancy = 0;
    pair<unsigned long, unsigned long> addr(s_dstAddr, s_srcAddr);
    Client& c = g_clients[addr];
    // Ignore clients we don't know anything about
    if (c.priority != 0) {
        updateSentBytes(c);
        // Approximate occupancy by its utilization of its assigned rate
        occupancy = (double)c.sentBytes / c.maxSentBytes;
        // Cap occupancy at 1
        if (occupancy > 1) {
            cout << "Capped occupancy " << occupancy << " to 1" << endl; // Shouldn't happen often, if at all
            occupancy = 1;
        }
        // Reset counters
        c.sentBytes = 0;
        c.maxSentBytes = 0;
    }
    return occupancy;
}

// UpdateClients RPC - update/add client configurations
void* net_enforcer_update_clients_svc(NetUpdateClientsArgs* argp, struct svc_req* rqstp)
{
    static char* result;
    for (unsigned int i = 0; i < argp->NetUpdateClientsArgs_len; i++) {
        const NetClientUpdate& clientUpdate = argp->NetUpdateClientsArgs_val[i];
        unsigned int priority = clientUpdate.priority;
        unsigned int rateLimitLength = clientUpdate.rateLimitRates.rateLimitRates_len;
        if (priority >= g_numPriorities) {
            cerr << "Invalid priority: " << priority << ", must be < " << g_numPriorities << endl;
            continue;
        }
        if (rateLimitLength > ((g_numLevels + 1) * 2)) {
            cerr << "Too many rate limits: " << rateLimitLength << ", must be <= " << ((g_numLevels + 1) * 2) << endl;
            continue;
        }
        updateClient(clientUpdate.client.s_dstAddr,
                     clientUpdate.client.s_srcAddr,
                     priority,
                     rateLimitLength,
                     clientUpdate.rateLimitRates.rateLimitRates_val,
                     clientUpdate.rateLimitBursts.rateLimitBursts_val);
    }
    return (void*)&result;
}

// RemoveClients RPC - remove clients
void* net_enforcer_remove_clients_svc(NetRemoveClientsArgs* argp, struct svc_req* rqstp)
{
    static char* result;
    for (unsigned int i = 0; i < argp->NetRemoveClientsArgs_len; i++) {
        const NetClient& client = argp->NetRemoveClientsArgs_val[i];
        // Special call to updateClient to cleanup client settings
        updateClient(client.s_dstAddr, client.s_srcAddr, g_numPriorities, 0, NULL, NULL);
    }
    return (void*)&result;
}

// GetOccupancy RPC - get occupancy statistics
NetGetOccupancyRes* net_enforcer_get_occupancy_svc(NetGetOccupancyArgs* argp, struct svc_req* rqstp)
{
    static NetGetOccupancyRes result;
    result.occupancy = getOccupancy(argp->s_dstAddr, argp->s_srcAddr);
    return &result;
}

// Main RPC handler
void net_enforcer_program(struct svc_req* rqstp, register SVCXPRT* transp)
{
    union {
        NetUpdateClientsArgs net_enforcer_update_clients_arg;
        NetRemoveClientsArgs net_enforcer_remove_clients_arg;
        NetGetOccupancyArgs net_get_occupancy_arg;
    } argument;
    char* result;
    xdrproc_t _xdr_argument, _xdr_result;
    char* (*local)(char*, struct svc_req*);

    switch (rqstp->rq_proc) {
        case NET_ENFORCER_NULL:
            svc_sendreply(transp, (xdrproc_t)xdr_void, (caddr_t)NULL);
            return;

        case NET_ENFORCER_UPDATE_CLIENTS:
            _xdr_argument = (xdrproc_t)xdr_NetUpdateClientsArgs;
            _xdr_result = (xdrproc_t)xdr_void;
            local = (char* (*)(char*, struct svc_req*))net_enforcer_update_clients_svc;
            break;

        case NET_ENFORCER_REMOVE_CLIENTS:
            _xdr_argument = (xdrproc_t)xdr_NetRemoveClientsArgs;
            _xdr_result = (xdrproc_t)xdr_void;
            local = (char* (*)(char*, struct svc_req*))net_enforcer_remove_clients_svc;
            break;

        case NET_ENFORCER_GET_OCCUPANCY:
            _xdr_argument = (xdrproc_t)xdr_NetGetOccupancyArgs;
            _xdr_result = (xdrproc_t)xdr_NetGetOccupancyRes;
            local = (char* (*)(char*, struct svc_req*))net_enforcer_get_occupancy_svc;
            break;

        default:
            svcerr_noproc(transp);
            return;
    }
    memset((char*)&argument, 0, sizeof(argument));
    if (!svc_getargs(transp, (xdrproc_t)_xdr_argument, (caddr_t)&argument)) {
        svcerr_decode(transp);
        return;
    }
    result = (*local)((char*)&argument, rqstp);
    if (result != NULL && !svc_sendreply(transp, (xdrproc_t)_xdr_result, result)) {
        svcerr_systemerr(transp);
    }
    if (!svc_freeargs(transp, (xdrproc_t)_xdr_argument, (caddr_t)&argument)) {
        cerr << "Unable to free arguments" << endl;
    }
}

// SIGTERM/SIGINT signal for cleanup
void term_signal(int signum)
{
    // Unregister NetEnforcer RPC handlers
    pmap_unset(NET_ENFORCER_PROGRAM, NET_ENFORCER_V1);
    // Remove TC root
    removeRoot();
    exit(0);
}

// Usage: ./NetEnforcer [-d dev] [-b maxBandwidth (in bytes per sec)] [-n numPriorities]
int main(int argc, char** argv)
{
    // Initialize globals
    int opt = 0;
    do {
        opt = getopt(argc, argv, "d:b:n:");
        switch (opt) {
            case 'd':
                g_dev.assign(optarg);
                break;

            case 'b':
                g_maxRate = atoi(optarg);
                break;

            case 'n':
                g_numPriorities = atoi(optarg);
                break;

            case -1:
                break;

            default:
                break;
        }
    } while (opt != -1);

    // Setup signal handler
    struct sigaction action;
    action.sa_handler = term_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);

    // Initialize TC
    initTC();

    // Unregister NetEnforcer RPC handlers
    pmap_unset(NET_ENFORCER_PROGRAM, NET_ENFORCER_V1);

    // Replace tcp RPC handlers
    register SVCXPRT *transp;
    transp = svctcp_create(RPC_ANYSOCK, 0, 0);
    if (transp == NULL) {
        cerr << "Failed to create tcp service" << endl;
        return 1;
    }
    if (!svc_register(transp, NET_ENFORCER_PROGRAM, NET_ENFORCER_V1, net_enforcer_program, IPPROTO_TCP)) {
        cerr << "Failed to register tcp NetEnforcer" << endl;
        return 1;
    }

    // Run proxy
    svc_run();
    cerr << "svc_run returned" << endl;
    return 1;
}
