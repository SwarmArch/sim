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

/* dsm: Minimalistic BFS example */
#include <stdlib.h>
#include <array>
#include <iostream>
#include <algorithm>
#include <deque>
#include <random>

struct Vertex {
    std::vector<Vertex*> adj;
    uint32_t level; // 0 == unconnected, 1 == root, etc

    Vertex() : level(0) {}
};

#ifdef COARSE_GRAIN_TASKS
void bfsTask(uint64_t level, Vertex* v);
//#define PLS_SINGLE_TASKFUNC bfsTask
//#define PLS_SINGLE_TASKFUNC_ARGS Vertex*
#endif

#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "common.h"


Vertex* GenRandomGraph(uint32_t numNodes, uint32_t avgDegree) {
    Vertex* graph = new Vertex[numNodes];
    std::default_random_engine gen(42 /* fixed seed */);
    std::geometric_distribution<uint32_t> degDist(1.0/avgDegree);
    std::uniform_int_distribution<int> adjDist(0, numNodes-1);

    for (uint32_t i = 0; i < numNodes; i++) {
        uint32_t degree = degDist(gen);
        graph[i].adj.resize(degree);
        for (Vertex*& n : graph[i].adj) n = &graph[adjDist(gen)];
    }
    return graph;
}

#ifdef COARSE_GRAIN_TASKS

// If def, parent task sets children's level and avoids duplicate tasks. Much
// more efficient (fewer tasks) for high-degree graphs, but causes more aborts
// and worse memory performance in Swarm

inline void bfsTask(uint64_t level, Vertex* v) {
    uint64_t nextLevel = level + 1;
    swarm::enqueue_all(v->adj.begin(), v->adj.end(), [nextLevel](Vertex* n) {
        if (!n->level) {
            n->level = nextLevel;
            swarm::enqueue(bfsTask, nextLevel, swarm::Hint::cacheLine(n), n);
        }
    }, level);
}
#else

#if 0
inline void bfsTask(uint64_t level, Vertex* v) {
    if (!v->level) {
        v->level = level;
        uint64_t nextLevel = level + 1;
        swarm::enqueue_all(v->adj.begin(), v->adj.end(), [nextLevel](Vertex* n) {
            swarm::enqueue(bfsTask, nextLevel, swarm::Hint::cacheLine(n), n);
        }, level);
    }
}
#else

// Fine-grained version, much lower queue footprint (one task per adjacency list)
inline void bfsTask(uint64_t level, Vertex* v);

inline void enqTask(uint64_t level, Vertex* v) {
    swarm::enqueue_all<NOHINT | MAYSPEC>(v->adj.begin(), v->adj.end(),
                                         [level](Vertex* n) {
        swarm::enqueue(bfsTask, level, {swarm::Hint::cacheLine(n), MAYSPEC}, n);
    }, level);
}

inline void bfsTask(uint64_t level, Vertex* v) {
    if (!v->level) {
        v->level = level;
        swarm::enqueue(enqTask, level+1, SAMEHINT | PRODUCER | MAYSPEC, v);
    }
}

#define CUSTOM_SPILLER 0
#if CUSTOM_SPILLER
struct TaskDescriptor { swarm::Timestamp ts; uint64_t vertex; };

static constexpr auto enqTaskFn =
    swarm::bareRunner<decltype(enqTask), enqTask, Vertex*>;

static void enqRequeuer(swarm::Timestamp, TaskDescriptor* tasks) {
    uint64_t numTasks = tasks[0].ts;
    TaskDescriptor *task = tasks + numTasks;
    for (; task > tasks; task--) {
        Vertex* v = (Vertex*)task->vertex;
        swarm::enqueue(enqTask, task->ts, SAMEHINT | MAYSPEC, v);
    }
    sim_zero_cycle_free(tasks);
}

// spiller (a.k.a. "coalescer") is enqueued naked (i.e., without a bareRunner)
__attribute__((noinline))
static void enqSpiller(swarm::Timestamp ts, const uint32_t n) {
    // Remove n oldest untied tasks from the tile and dump them into memory
    TaskDescriptor* tasks = (TaskDescriptor*)sim_zero_cycle_untracked_malloc(
        sizeof(TaskDescriptor) * (n + 1));
    TaskDescriptor* task = tasks + 1;

    swarm::Timestamp minTs = UINT64_MAX;
    for (; task < tasks + n+1; task++) {
        constexpr uint64_t magicOp = MAGIC_OP_REMOVE_UNTIED_TASKFN;
        uint64_t ts;
        uint64_t vertex;
        COMPILER_BARRIER();
        // For some reason passing the MAGIC_OP as an input to the xchg rcx
        // rcx instruction requires one extra register to hold the MAGIC_OP
        // before moving it into rcx. The following forces the compiler to
        // move the MAGIC_OP directly into rcx, releliving register pressure
        // by one.
        asm volatile("mov %0, %%rcx;" : : "g"(magicOp) :);
        asm volatile("xchg %%rcx, %%rcx;"
                     : "=c"(ts), "=D"(vertex)
                     : "D"(minTs), "S"(uint64_t(enqTaskFn))
                     :);
        COMPILER_BARRIER();

        if (ts == UINT64_MAX) break;

        task->ts = ts;
        task->vertex = vertex;

        minTs = ts;
    }

    const uint64_t numTasks = (task - tasks) - 1;
    tasks[0].ts = numTasks;

    if (numTasks > 0) {
        EnqFlags ef = SAMEHINT | NOHASH | PRODUCER | REQUEUER;
        swarm::enqueue(enqRequeuer, minTs, ef, tasks);
    }
}
#endif  // CUSTOM_SPILLER

#endif
#endif

int main(int argc, const char** argv) {
    if (argc != 3) {
        printf("Usage: %s <numNodes> <avgDegree>\n", argv[0]);
        return -1;
    }

    uint32_t numNodes = atoi(argv[1]);
    uint32_t avgDegree = atoi(argv[2]);

    Vertex* graph = GenRandomGraph(numNodes, avgDegree);
#ifdef COARSE_GRAIN_TASKS
    graph[0].level = 1;
    swarm::enqueue(bfsTask, 1, swarm::Hint::cacheLine(&graph[0]), &graph[0]);
#else
#if CUSTOM_SPILLER
    sim_magic_op_2(MAGIC_OP_REGISTER_SPILLER, (uint64_t)enqSpiller, (uint64_t)enqTaskFn);
#endif  // CUSTOM_SPILLER
    swarm::enqueue(bfsTask, 1,
                 {swarm::Hint::cacheLine(&graph[0]), MAYSPEC},
                 &graph[0]);
#endif
    swarm::run();

    // Verification
    uint32_t treeNodes = 0;
    uint32_t curLevel = 1;
    uint64_t sumLevels = 0;

    bool success = true;
    std::deque<Vertex*> fringe = {&graph[0]};
    std::vector<bool> inserted;
    inserted.resize(numNodes);
    inserted[0] = true;

    while(!fringe.empty()) {
        Vertex* v = fringe.front();
        fringe.pop_front();
        success &= (v->level == curLevel || v->level == curLevel+1);
        assert(success);
        uint32_t idx = v - graph;
        success = inserted[idx];
        assert(success);
        curLevel = v->level;
        for (Vertex* n : v->adj) {
            uint32_t idx = n - graph;
            if (!inserted[idx]) {
                fringe.push_back(n);
                inserted[idx] = true;
            }
        }

        treeNodes++;
        sumLevels += curLevel;
    }

    printf("Resulting BFS tree: %d nodes, avg level %.2f (root = 1), max level %d\n",
            treeNodes, 1.0*sumLevels/treeNodes, curLevel);

    tests::assert_true(success);
    return 0;
}
