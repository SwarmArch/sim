/** $lic$
 * Copyright (C) 2014-2021 by Massachusetts Institute of Technology
 *
 * This file is part of the Swarm simulator.
 *
 * This simulator is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, version 2.
 *
 * This simulator was developed as part of the Swarm architecture project. If
 * you use this software in your research, we request that you reference the
 * Swarm MICRO 2018 paper ("Harmonizing Speculative and Non-Speculative
 * Execution in Architectures for Ordered Parallelism", Jeffrey et al.,
 * MICRO-51, 2018) as the source of this simulator in any publications that use
 * this software, and that you send us a citation of your work.
 *
 * This simulator is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <bitset>
#include <deque>
#include <memory>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <vector>
#include <utility>
#include "sim/assert.h"
#include "sim/atomic_port.h"
#include "sim/bit_window.h"
#include "sim/init/config.h"  // for ParseList
#include "sim/event.h"
#include "sim/log.h"
#include "sim/object.h"
#include "sim/port.h"
#include "sim/stats/stats.h"
#include "sim/stats/table_stat.h"
#include "sim/str.h"
#include "sim/collections/enum.h"
#include "sim/network_msg_types.h"

class Network : public SimObject {
  private:
    const uint32_t numNodes;
    const uint32_t xdim;
    const uint32_t ydim;
    const uint32_t routerDelay;
    const uint32_t linkDelay;
    const uint32_t expressLinkHops;
    const uint32_t expressLinkDelay;
    const uint32_t linkBytes;

    // Network-port interposition
    template <typename T, typename R>
    class AtomicNetPort : public AtomicPort<T, R> {
      private:
        AtomicPort<T, R>& endpoint;
        Network& net;
        uint32_t srcNode;
        uint32_t dstNode;
        uint32_t reqPairId;
        uint32_t respPairId;

      public:
        AtomicNetPort(AtomicPort<T, R>& _endpoint, Network& _net,
                      uint32_t _srcNode, uint32_t _dstNode, uint32_t _reqPairId,
                      uint32_t _respPairId)
            : endpoint(_endpoint),
              net(_net),
              srcNode(_srcNode),
              dstNode(_dstNode),
              reqPairId(_reqPairId),
              respPairId(_respPairId) {}

        uint64_t access(T& req, R& resp, uint64_t cycle) {
            cycle = net.traverse(srcNode, dstNode, req.bytes(), reqPairId,
                                 cycle, getIdx(req));
            cycle = endpoint.access(req, resp, cycle);
            cycle = net.traverse(dstNode, srcNode, resp.bytes(req), respPairId,
                                 cycle, getIdx(req, resp));
            return cycle;
        }

      private:
        static uint32_t getIdx(T& req) { return Helper<T>::typeToIdx(req); }
        static uint32_t getIdx(T& req, R& resp) { return HelperResp<T,R>::typeToIdx(req,resp); }
    };

    template <typename T>
    class LocalPort : public Port<T> {
      public:
          typedef typename Port<T>::Receiver LocalReceiver;
      private:
          LocalReceiver& endpoint;

      public:
        LocalPort(LocalReceiver& _endpoint) : endpoint(_endpoint) {}

        void send(const T& msg, uint64_t cycle) override {
            LocalReceiver& r = endpoint;
            // dsm: I had to look this up... per the standard, capturing a
            // reference by value (msg) copies the value. Capturing a reference
            // by reference (&r) is less clear, though it almost surely creates
            // a reference bound to the lifetime of the first reference. If
            // this fails, we can capture an std::cref by value, see
            // http://stackoverflow.com/questions/23067111/lambda-capture-reference-variable-by-reference
            schedEvent(cycle,
                       [&r, msg] (uint64_t cycle) { r.receive(msg, cycle); });
        }
    };

    template <typename T>
    class NetPort : public Port<T> {
      public:
          typedef typename Port<T>::Receiver NetReceiver;
      private:
          NetReceiver& endpoint;
          Network& net;
          uint32_t srcNode;
          uint32_t dstNode;
          uint32_t pairId;

      public:
        NetPort(NetReceiver& _endpoint, Network& _net, uint32_t _srcNode,
                uint32_t _dstNode, uint32_t _pairId)
            : endpoint(_endpoint),
              net(_net),
              srcNode(_srcNode),
              dstNode(_dstNode),
              pairId(_pairId) {}

        void send(const T& msg, uint64_t cycle) override {
            schedEvent(cycle, [this, msg](uint64_t cycle) {
                // NOTE: Could make traversal more fine-grained...
                cycle = net.traverse(srcNode, dstNode, msg.bytes(), pairId,
                                     cycle, getIdx(msg));
                schedEvent(cycle, [this, msg](uint64_t cycle) {
                    endpoint.receive(msg, cycle);
                });
            });
        }

      private:
        static uint32_t getIdx(const T& msg) { return Helper<T>::typeToIdx(msg); }
    };

    // Endpoint registration and lookup structures
    struct Endpoint {
        SimObject* obj;
        uint32_t node;
        uint32_t groupId;
    };

    std::vector<Endpoint> endpoints;
    std::unordered_map<SimObject*, uint32_t> objectToEndpointId;

    // Groups and pairs, used for profiling
    std::vector<std::string> groupNames;
    std::vector<std::string> pairNames;

    // 2 levels --> 64^2 = 4Kcycles of acceleration...
    // If this is a bottleneck, could increase to 256K, but seems pointless
    typedef BitWindow<2> Link;

    struct SubNetwork {
        std::vector<Link> links;
        std::vector<std::array<Link*, 8>> nodeLinks;
    };

    std::vector<SubNetwork> subnets;

    const bool straightFastpath;
    const bool contention;

    // Stats
    RunningStat<size_t> flitsPerPacket;
    RunningStat<size_t> cyclesPerPacket;
    RunningStat<size_t> contentionCyclesPerPacket;
    VectorCounter linkFlits;
    VectorCounter pairInjectedFlits;
    VectorCounter pairTotalFlits;
    TableStat pairInjectedFlitsByType;
    TableStat pairTotalFlitsByType;

  public:
    Network(const std::string _name, uint32_t _numNodes, uint32_t _xdim,
            uint32_t _routerDelay, uint32_t _linkDelay,
            uint32_t _expressLinkHops, uint32_t _expressLinkDelay,
            uint32_t _linkBytes, bool _straightFastpath, uint32_t _numSubnets,
            bool _contention);

    void initStats(AggregateStat* parentStat);

    void getNeighbors(uint32_t nodeId, uint32_t hops,
                      std::vector<uint32_t>& neighbors);

    // NOTE: Templated to avoid casting vector elems, but you must pass
    // SimObject subclasses here
    template <typename T>
    void addEndpoints(const std::string& groupName, const std::vector<T*>& objs,
                      const std::string& nodeStr) {
        // Check input conditions
        if (!objs.size()) panic("addEndpoints() can't take an empty group");

        std::vector<uint32_t> nodes;
        if (nodeStr == "") {
            if (objs.size() % numNodes) {
                panic(
                    "%s has %ld objects, not multiple of %d nodes, don't "
                    "know how to map implicitly. Did you specify nodes in "
                    "config?",
                    groupName.c_str(), objs.size(), numNodes);
            }

            for (uint32_t i = 0; i < objs.size(); i++) {
                nodes.push_back(i * numNodes / objs.size());
            }
        } else if (nodeStr == "center") {
            // Compute nodes in the center (1, 2, or 4)
            uint32_t xcenter[(xdim % 2)? 1 : 2];
            uint32_t ycenter[(ydim % 2)? 1 : 2];
            xcenter[0] = (xdim - 1) / 2;
            if (!(xdim % 2)) xcenter[1] = xdim / 2;
            ycenter[0] = (ydim - 1) / 2;
            if (!(ydim % 2)) ycenter[1] = ydim / 2;

            std::vector<uint32_t> centerNodes;
            for (auto x : xcenter) for (auto y: ycenter) {
                centerNodes.push_back(nodeId(x, y));
            }

            // Spread around center nodes
            for (uint32_t i = 0; i < objs.size(); i++) {
                nodes.push_back(centerNodes[i % centerNodes.size()]);
            }
        } else if (nodeStr == "edges") {
            // Compute nodes in the edges
            // Examples:
            //
            //   0  1  2  3
            //   4  5  6  7
            //   8  9 10 11
            //  12 13 14 15
            //  edgeNodes = 0 1 2 3 7 11 15 14 13 12 8 4
            //
            //   0  1  2  3
            //   4  5  6  7
            //   8  9 10 11
            //  12 13
            //  edgeNodes = 0 1 2 3 7 11 10  9 13 12 8 4
            std::vector<uint32_t> edgeNodes;
            // Special case to handle 1 node case
            if (xdim == 1 && ydim == 1) {
                edgeNodes.push_back(nodeId(0, 0));
            }
            for (uint32_t x = 0; x < xdim-1; x++) {
                edgeNodes.push_back(nodeId(x, 0));
            }
            for (uint32_t y = 0; y < ydim-1; y++) {
                edgeNodes.push_back(nodeId(xdim-1, y));
            }
            if (xdim > 1) for (uint32_t x = 0; x < xdim-1; x++) {
                // Because numNodes might be < xdim*ydim, pull up to get
                // to an existing node (see example above)
                uint32_t n = nodeId(xdim-1-x, ydim-1);
                if (n >= numNodes) n -= xdim + 1;
                assert(n < numNodes);
                edgeNodes.push_back(n);
            }
            if (ydim > 1) for (uint32_t y = 0; y < ydim-1; y++) {
                edgeNodes.push_back(nodeId(0, ydim-1-y));
            }

            // Distribute nodes uniformly. This will work in any case, but may
            // produce somewhat off-balance partitionings (e.g., in non-square
            // meshes, 4 nodes won't be in the corners)
            for (uint32_t i = 0; i < objs.size(); i++) {
                nodes.push_back(edgeNodes[i * edgeNodes.size() / objs.size()]);
            }
        } else {  // interpret as a list
            nodes = ParseList<uint32_t>(nodeStr);
            if (objs.size() != nodes.size()) {
                panic("%s has %ld objects != %ld specified nodes",
                      groupName.c_str(), objs.size(), nodes.size());
            }
            for (auto n : nodes)
                if (n >= numNodes) {
                    panic("%s node %d out of range (max %d)", groupName.c_str(),
                          n, numNodes);
                }
        }
        // info("%s mapped to nodes %s", groupName.c_str(), Str(nodes).c_str());
        assert(nodes.size() == objs.size());

        // Allocate group
        uint32_t groupId = groupNames.size();
        for (auto gn : groupNames) assert(gn != groupName);
        groupNames.push_back(groupName);

        for (uint32_t i = 0; i < objs.size(); i++) {
            endpoints.push_back({objs[i], nodes[i], groupId});
            objectToEndpointId.insert({objs[i], endpoints.size()-1});
        }
    }

    template <typename P> P* getAtomicPort(SimObject* src, SimObject* obj) {
        uint32_t srcNode = endpoints[endpointId(src)].node;
        const Endpoint& dst = endpoints[endpointId(obj)];
        P* port = dynamic_cast<P*>(obj);
        if (!port) {
            panic("getPort(): %s is not a %s", obj->name(), typeid(P).name());
        }
        if (srcNode == dst.node) {
            return port;
        } else {
            uint32_t reqPairId = getOrCreatePairId(src, obj);
            uint32_t respPairId = getOrCreatePairId(obj, src);
            return new AtomicNetPort<typename P::req_type,
                                     typename P::resp_type>(
                *port, *this, srcNode, dst.node, reqPairId, respPairId);
        }
    }

    template <typename P> P* getPort(SimObject* src, SimObject* obj) {
        typedef typename P::Receiver Receiver;
        uint32_t srcNode = endpoints[endpointId(src)].node;
        const Endpoint& dst = endpoints[endpointId(obj)];
        Receiver* receiver = dynamic_cast<Receiver*>(obj);
        if (!receiver) {
            panic("getPort(): %s is not a %s", obj->name(),
                  typeid(Receiver).name());
        }
        if (srcNode == dst.node) {
            return new LocalPort<typename P::msg_type>(*receiver);
        } else {
            uint32_t pairId = getOrCreatePairId(src, obj);
            return new NetPort<typename P::msg_type>(*receiver, *this, srcNode,
                                                     dst.node, pairId);
        }
    }

    template <typename P, typename T>
    std::vector<P*> getAtomicPorts(SimObject* src,
                                   const std::vector<T*>& objs) {
        std::vector<P*> ports;
        ports.resize(objs.size());
        for (uint32_t i = 0; i < objs.size(); i++) {
            ports[i] = getAtomicPort<P>(src, objs[i]);
        }
        return ports;
    }

    template <typename P, typename T>
    std::vector<P*> getPorts(SimObject* src, const std::vector<T*>& objs) {
        std::vector<P*> ports;
        ports.resize(objs.size());
        for (uint32_t i = 0; i < objs.size(); i++) {
            ports[i] = getPort<P>(src, objs[i]);
        }
        return ports;
    }

    uint32_t getNode(SimObject* obj) const {
        return endpoints[endpointId(obj)].node;
    }

    uint32_t getCenterNode() const {
        return nodeId((xdim - 1) / 2, (ydim - 1) / 2);
    }

    std::vector<uint32_t> getNeighbors(uint32_t node);

  private:
    uint64_t traverse(uint32_t srcId, uint32_t dstId, uint32_t bytes, uint32_t pairId, uint64_t cycle);
    uint64_t traverse(uint32_t srcId, uint32_t dstId, uint32_t bytes,
                      uint32_t pairId, uint64_t cycle, uint32_t typeIdx);

    inline uint32_t endpointId(SimObject* obj) const {
        auto it = objectToEndpointId.find(obj);
        if (it == objectToEndpointId.end()) {
            panic("[%s] Tried lookup on non-registered object %s", name(),
                  obj->name());
        }
        return it->second;
    }

    inline std::tuple<uint32_t, uint32_t> nodeLoc(uint32_t nodeId) const {
        uint32_t xloc = nodeId % xdim;
        uint32_t yloc = nodeId / xdim;
        return std::make_tuple(xloc, yloc);
    }

    inline uint32_t nodeId(uint32_t xloc, uint32_t yloc) const {
        return xloc + yloc * xdim;
    }

    inline uint32_t getOrCreatePairId(SimObject* src, SimObject* dst) {
        auto gs = [this](SimObject* o) {
            return groupNames[endpoints[endpointId(o)].groupId];
        };
        std::string pairName = gs(src) + "->" + gs(dst);
        auto it = std::find(pairNames.begin(), pairNames.end(), pairName);
        if (it == pairNames.end()) {
            pairNames.push_back(pairName);
            return pairNames.size()-1;
        } else {
            return &(*it) - &pairNames[0];
        }
    }
};
