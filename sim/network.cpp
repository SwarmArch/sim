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

#include "sim/network.h"
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <random>
#include "sim/sim.h"
#include "sim/str.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

// NOTE: NetDirection enum coded so that it's simple to get flags out of it
// (see isHorizontal() etc. below)
enum NetDirection {
    ND_N = 0, ND_W = 1, ND_S = 2, ND_E = 3,  // short links
    ND_NX = 4, ND_WX = 5, ND_SX = 6, ND_EX = 7,  // express links
};

static inline bool isHorizontal(NetDirection dir) { return dir & 1; }
static inline bool isPositiveDelta(NetDirection dir) { return dir & 2; }
static inline bool isExpress(NetDirection dir) { return dir & 4; }

static std::mt19937 subnetGen(0xF337);
static std::uniform_int_distribution<uint32_t> subnetDis(0, UINT32_MAX);

static std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                  uint32_t, uint32_t, uint32_t, uint32_t>
_traversalParameters(uint32_t curx, uint32_t cury, uint32_t dstx, uint32_t dsty,
                     uint32_t bytes, uint32_t linkBytes,
                     uint32_t expressLinkHops) {
    uint32_t flits = bytes / linkBytes;
    flits += (flits * linkBytes != bytes);

    uint32_t distx = std::abs((int32_t)curx - (int32_t)dstx);
    uint32_t disty = std::abs((int32_t)cury - (int32_t)dsty);

    uint32_t shortHopsx = expressLinkHops? distx % expressLinkHops : distx;
    uint32_t expressHopsx = expressLinkHops? distx / expressLinkHops : 0;
    uint32_t shortHopsy = expressLinkHops? disty % expressLinkHops : disty;
    uint32_t expressHopsy = expressLinkHops? disty / expressLinkHops : 0;

    uint32_t shortHops = shortHopsx + shortHopsy;
    uint32_t expressHops = expressHopsx + expressHopsy;
    uint32_t hops = shortHops + expressHops;

    return std::make_tuple(flits, distx, disty, hops,
                           shortHops, shortHopsx, shortHopsy, expressHops,
                           expressHopsx, expressHopsy);
}

Network::Network(const std::string _name, uint32_t _numNodes, uint32_t _xdim,
                 uint32_t _routerDelay, uint32_t _linkDelay,
                 uint32_t _expressLinkHops, uint32_t _expressLinkDelay,
                 uint32_t _linkBytes, bool _straightFastpath,
                 uint32_t _numSubnets, bool _contention)
    : SimObject(_name),
      numNodes(_numNodes),
      xdim(_xdim),
      ydim(std::ceil((1. * numNodes) / xdim)),
      routerDelay(_routerDelay),
      linkDelay(_linkDelay),
      expressLinkHops(_expressLinkHops),
      expressLinkDelay(_expressLinkDelay),
      linkBytes(_linkBytes),
      straightFastpath(_straightFastpath),
      contention(_contention) {
    assert(linkBytes);
    assert(xdim && ydim);
    assert(xdim * ydim >= numNodes);
    // NOTE: No upper bound on expressLinkHops b/c it makes config easier (can
    // keep constant on smaller networks)
    assert(expressLinkHops != 1);
    DEBUG("[%s] Simulating a %dx%d network (%d nodes)", name(), xdim, ydim,
          numNodes);

    if (numNodes == 1) return;

    uint32_t numLinks = xdim*ydim*4 - 2*(xdim + ydim);
    if (expressLinkHops) {
        // Number of express links is easy to compute by induction: with n
        // nodes per dim and h hops per express link, there are 0 express links
        // if n <= h, and increasing n by 1 adds 1 express link to the dim.
        uint32_t lx = (expressLinkHops < xdim)? xdim - expressLinkHops : 0;
        uint32_t ly = (expressLinkHops < ydim)? ydim - expressLinkHops : 0;
        numLinks += lx*ydim*2 + ly*xdim*2;
    }

    DEBUG("%d links, %d subnets", numLinks, _numSubnets);
    std::vector<std::string> linkNames;

    subnets.resize(_numSubnets);
    for (uint32_t s = 0; s < subnets.size(); s++) {
        linkNames.clear();
        auto& links = subnets[s].links;
        auto& nodeLinks = subnets[s].nodeLinks;

        links.resize(numLinks);
        nodeLinks.resize(xdim*ydim);
        uint32_t curLink = 0;
        for (uint32_t n = 0; n < xdim*ydim; n++) {
            uint32_t x, y;
            std::tie(x, y) = nodeLoc(n);
            bool skipN = y == 0;
            bool skipW = x == 0;
            bool skipS = y == ydim - 1;
            bool skipE = x == xdim - 1;
            bool skipNX = !expressLinkHops || (y < expressLinkHops);
            bool skipWX = !expressLinkHops || (x < expressLinkHops);
            bool skipSX = !expressLinkHops || (y + expressLinkHops >= ydim);
            bool skipEX = !expressLinkHops || (x + expressLinkHops >= xdim);
            nodeLinks[n][ND_N] = skipN ? nullptr : &links[curLink++];
            nodeLinks[n][ND_W] = skipW ? nullptr : &links[curLink++];
            nodeLinks[n][ND_S] = skipS ? nullptr : &links[curLink++];
            nodeLinks[n][ND_E] = skipE ? nullptr : &links[curLink++];
            nodeLinks[n][ND_NX] = skipNX ? nullptr : &links[curLink++];
            nodeLinks[n][ND_WX] = skipWX ? nullptr : &links[curLink++];
            nodeLinks[n][ND_SX] = skipSX ? nullptr : &links[curLink++];
            nodeLinks[n][ND_EX] = skipEX ? nullptr : &links[curLink++];

            std::string lp = "link" + Str(n) + "_";
            if (!skipN) linkNames.push_back(lp + Str(n - xdim));
            if (!skipW) linkNames.push_back(lp + Str(n - 1));
            if (!skipS) linkNames.push_back(lp + Str(n + xdim));
            if (!skipE) linkNames.push_back(lp + Str(n + 1));
            if (!skipNX) linkNames.push_back(lp + Str(n - expressLinkHops*xdim));
            if (!skipWX) linkNames.push_back(lp + Str(n - expressLinkHops));
            if (!skipSX) linkNames.push_back(lp + Str(n + expressLinkHops*xdim));
            if (!skipEX) linkNames.push_back(lp + Str(n + expressLinkHops));
            //info("%3d (%2d,%2d) %d%d%d%d %d%d%d%d", n, x, y, !skipN, !skipW, !skipS,
            //     !skipE, !skipNX, !skipWX, !skipSX, !skipEX);
        }
        assert(curLink == numLinks);
    }
    // FIXME: Kludgy, done here because I don't want to pass linkNames around
    linkFlits.init("linkFlits", "Flits through each link", linkNames);
}

void Network::initStats(AggregateStat* parentStat) {
    if (numNodes == 1) return;

    AggregateStat* netStat = new AggregateStat();
    netStat->init(name(), "Network stats");

    flitsPerPacket.init("flitsPerPacket", "flits/packet");
    cyclesPerPacket.init("cyclesPerPacket", "end-to-end cycles/packet");
    contentionCyclesPerPacket.init("contentionCyclesPerPacket", "end-to-end contention delay/packet");
    pairInjectedFlits.init("pairInjectedFlits", "Flits injected between src->dst pairs", pairNames);
    pairTotalFlits.init("pairTotalFlits", "Total flits moved between src->dst pairs", pairNames);
    const std::vector<std::string> typeNames =
            {"GETS", "GETX", "PUTS", "PUTX", // for MemReq
             "INV", "INVX", "FWD",  // for InvReq
             "MEMDATA", "MEMACK",   // for both MemResp and InvResp
             "ABORTREQ", "ABORTACK",
             "ENQREQ", "ENQRESP",
             "CUTTIE",
             "GVT", "LVT"
            };
    pairInjectedFlitsByType.init(
        "pairInjectedFlitsByType",
        "Flits injected between src->dst pairs by type",
        pairNames, typeNames);
    pairTotalFlitsByType.init(
        "pairTotalFlitsByType",
        "Total flits moved between src->dst pairs by type",
        pairNames, typeNames);
    netStat->append(&flitsPerPacket);
    netStat->append(&cyclesPerPacket);
    netStat->append(&contentionCyclesPerPacket);
    netStat->append(&pairInjectedFlits);
    netStat->append(&pairTotalFlits);
    netStat->append(&pairInjectedFlitsByType);
    netStat->append(&pairTotalFlitsByType);
    netStat->append(&linkFlits);  // initialized in constructor
    parentStat->append(netStat);
}

uint64_t Network::traverse(uint32_t srcId, uint32_t dstId, uint32_t bytes,
                           uint32_t pairId, uint64_t cycle) {
    uint32_t curNode = srcId;
    uint32_t curx, cury, dstx, dsty;
    std::tie(curx, cury) = nodeLoc(curNode);
    std::tie(dstx, dsty) = nodeLoc(dstId);

    uint32_t distx, disty;
    uint32_t flits, hops;
    uint32_t shortHops, shortHopsx, shortHopsy;
    uint32_t expressHops, expressHopsx, expressHopsy;

    std::tie(flits, distx, disty, hops, shortHops, shortHopsx, shortHopsy,
             expressHops, expressHopsx, expressHopsy) =
        _traversalParameters(curx, cury, dstx, dsty, bytes, linkBytes,
                             expressLinkHops);

    flitsPerPacket.push(flits, 1 /*weight = 1 packet*/);
    pairInjectedFlits.inc(pairId, flits);
    pairTotalFlits.inc(pairId, flits * hops);

    uint32_t routerTraversals = hops + 1;
    if (straightFastpath) {
        routerTraversals =
            2 /* initial injection and final ejection */ +
            (distx && disty) /* dimension turn */ +
            (shortHopsx && expressHopsx) /* x-dim express-to-short*/ +
            (shortHopsy && expressHopsy) /* y-dim express-to-short*/;
    }

    uint32_t minDelay = routerTraversals * routerDelay + shortHops * linkDelay +
                        expressHops * expressLinkDelay +
                        (flits - 1) /* serialization */;
    assert(minDelay > 0);

    // Select subnet
    uint32_t subnet = (contention && subnets.size() > 1)? (subnetDis(subnetGen) % subnets.size()) : 0;
    auto& links = subnets[subnet].links;
    auto& nodeLinks = subnets[subnet].nodeLinks;

    // Gather & advance all links in the route (XY routing)
    Link* route[hops];
    uint32_t hopDelay[hops]; // link and next router
    uint32_t curHop = 0;

    auto addHop = [&](NetDirection dir, bool lastInDim) {
        assert(curHop < hops);
        route[curHop] = nodeLinks[curNode][dir];
        assert_msg(route[curHop], "Null link, node %d (%d, %d), dir %d",
                   curNode, curx, cury, dir);
        if (contention) route[curHop]->advance(getCurCycle());
        bool traverseRouter = !straightFastpath || lastInDim;
        hopDelay[curHop] = (isExpress(dir) ? expressLinkDelay : linkDelay) +
                           traverseRouter * routerDelay;
        curHop++;

        int32_t delta = (isPositiveDelta(dir) ? +1 : -1) *
                        (isExpress(dir) ? expressLinkHops : 1);
        if (isHorizontal(dir)) {
            curx += delta;
            curNode += delta;
            assert(curx < xdim);
        } else {
            cury += delta;
            curNode += delta*xdim;
            assert(cury < ydim);
        }
        assert(curNode < xdim*ydim);
    };

    NetDirection dirLX = curx > dstx ? ND_WX : ND_EX;
    NetDirection dirSX = curx > dstx ? ND_W : ND_E;
    NetDirection dirLY = cury > dsty ? ND_NX : ND_SX;
    NetDirection dirSY = cury > dsty ? ND_N : ND_S;
    for (uint32_t h = 0; h < expressHopsx; h++) addHop(dirLX, h == expressHopsx - 1);
    for (uint32_t h = 0; h < shortHopsx; h++) addHop(dirSX, h == shortHopsx - 1);
    for (uint32_t h = 0; h < expressHopsy; h++) addHop(dirLY, h == expressHopsy - 1);
    for (uint32_t h = 0; h < shortHopsy; h++) addHop(dirSY, h == shortHopsy - 1);

    assert(curHop == hops);
    assert(curx == dstx);
    assert(cury == dsty);

    // Profile link activity
    for (Link* link : route) {
        uint32_t n = link - &links[0];
        assert(n < links.size());
        linkFlits.inc(n, flits);
    }

    if (!contention) {
        cyclesPerPacket.push(minDelay, 1);
        return cycle + minDelay;
    } else {
        // Find delays w/ contention---schedule all flits hop by hop
        assert(flits);
        uint64_t flitCycles[flits];
        for (uint32_t f = 0; f < flits; f++) {
            flitCycles[f] = cycle + routerDelay + f;
        }
        for (uint32_t h = 0; h < hops; h++) {
            for (uint32_t f = 0; f < flits; f++) {
                // [mcj] Advance the cycle due to contention
                flitCycles[f] = route[h]->schedule(flitCycles[f]);
                flitCycles[f] += hopDelay[h];
            }
        }
        uint64_t curCycle = flitCycles[flits-1];
        uint32_t delay = curCycle - cycle;
        assert(delay >= minDelay);
        if (delay > 1000*minDelay)
            filtered_info("WARN: packet took %u cycles to traverse network, "
                          "when it would have taken %u cycles with no contention.",
                          delay, minDelay);
        cyclesPerPacket.push(delay, 1);
        contentionCyclesPerPacket.push(delay - minDelay, 1);
        assert(curCycle >= cycle);
        return curCycle;
    }
}

uint64_t Network::traverse(uint32_t srcId, uint32_t dstId, uint32_t bytes,
                           uint32_t pairId, uint64_t cycle, uint32_t typeId) {
    uint32_t curNode = srcId;
    uint32_t curx, cury, dstx, dsty;
    std::tie(curx, cury) = nodeLoc(curNode);
    std::tie(dstx, dsty) = nodeLoc(dstId);

    uint32_t distx, disty;
    uint32_t flits, hops;
    uint32_t shortHops, shortHopsx, shortHopsy;
    uint32_t expressHops, expressHopsx, expressHopsy;

    std::tie(flits, distx, disty, hops, shortHops, shortHopsx, shortHopsy,
             expressHops, expressHopsx, expressHopsy) =
        _traversalParameters(curx, cury, dstx, dsty, bytes, linkBytes,
                             expressLinkHops);

    pairInjectedFlitsByType.inc(pairId, typeId, flits);
    pairTotalFlitsByType.inc(pairId, typeId, flits * hops);

    return traverse(srcId, dstId, bytes, pairId, cycle);
}

// FIXME(dsm): This should not be needed, and the only user of this method
// looks like dead code.
void Network::getNeighbors(uint32_t node, uint32_t hops,
                           std::vector<uint32_t>& neighbors) {
    for (uint32_t x = 0; x <= hops; ++x) {
        for (uint32_t y = (x == 0) ? 1 : 0; y <= (hops - x); ++y) {
            assert(x + y <= hops);
            uint32_t thisNodeX, thisNodeY;
            std::tie(thisNodeX, thisNodeY) = nodeLoc(node);

            auto push = [&, this](uint32_t n) {
                if (n < numNodes) neighbors.push_back(n);
            };

            if ((thisNodeX + x < xdim) && (thisNodeY + y < ydim))
                push(nodeId(thisNodeX + x, thisNodeY + y));
            if ((thisNodeX - x < xdim) && (thisNodeY + y < ydim))
                push(nodeId(thisNodeX - x, thisNodeY + y));
            if ((thisNodeX + x < xdim) && (thisNodeY - y < ydim))
                push(nodeId(thisNodeX + x, thisNodeY - y));
            if ((thisNodeX - x < xdim) && (thisNodeY - y < ydim))
                push(nodeId(thisNodeX - x, thisNodeY - y));
        }
    }
    std::sort(neighbors.begin(), neighbors.end());
    neighbors.erase(std::unique(neighbors.begin(), neighbors.end()),
                    neighbors.end());
}

std::vector<uint32_t> Network::getNeighbors(uint32_t node) {
    uint32_t x, y;
    std::tie(x, y) = nodeLoc(node);
    std::vector<uint32_t> neighs;
    for (uint32_t d = 0; d < 8; d++) {
        NetDirection dir = (NetDirection) d;

        int32_t delta = isPositiveDelta(dir) ? 1 : -1;
        if (isExpress(dir)) delta *= expressLinkHops;

        uint32_t nx = x;
        uint32_t ny = y;
        if (isHorizontal(dir)) nx += delta;
        else ny += delta;

        if (nx >= xdim || ny >= ydim) continue;
        uint32_t n = nodeId(nx, ny);
        if (n >= numNodes) continue;

        neighs.push_back(n);
    }
    return neighs;
}
