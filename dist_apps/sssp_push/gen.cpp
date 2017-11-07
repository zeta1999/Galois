/** SSSP -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2013, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @section Description
 *
 * Compute Single Source Shortest Path on distributed Galois using worklist.
 *
 * @author Gurbinder Gill <gurbinder533@gmail.com>
 * @author Roshan Dathathri <roshan@cs.utexas.edu>
 * @author Loc Hoang <l_hoang@utexas.edu> (sanity check operators)
 */

#include <iostream>
#include <limits>
#include "galois/DistGalois.h"
#include "galois/gstl.h"
#include "DistBenchStart.h"
#include "galois/DistAccumulator.h"
#include "galois/runtime/Tracer.h"

#ifdef __GALOIS_HET_CUDA__
#include "gen_cuda.h"
struct CUDA_Context *cuda_ctx;
#endif

constexpr static const char* const REGION_NAME = "SSSP";

/******************************************************************************/
/* Declaration of command line arguments */
/******************************************************************************/

namespace cll = llvm::cl;

static cll::opt<unsigned int> maxIterations("maxIterations", 
                                            cll::desc("Maximum iterations: "
                                                      "Default 1000"), 
                                            cll::init(1000));
static cll::opt<unsigned long long> src_node("srcNodeId", 
                                             cll::desc("ID of the source node"), 
                                             cll::init(0));

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

const uint32_t infinity = std::numeric_limits<uint32_t>::max()/4;

struct NodeData {
  std::atomic<uint32_t> dist_current;
  uint32_t dist_old;
};

galois::DynamicBitSet bitset_dist_current;

typedef hGraph<NodeData, unsigned int> Graph;
typedef typename Graph::GraphNode GNode;

#include "gen_sync.hh"

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/

struct InitializeGraph {
  const uint32_t &local_infinity;
  cll::opt<unsigned long long> &local_src_node;
  Graph *graph;

  InitializeGraph(cll::opt<unsigned long long> &_src_node, 
                  const uint32_t &_infinity, Graph* _graph) : 
      local_infinity(_infinity), local_src_node(_src_node), graph(_graph){}

  void static go(Graph& _graph) {
    const auto& allNodes = _graph.allNodesRange();

    #ifdef __GALOIS_HET_CUDA__
      if (personality == GPU_CUDA) {
        std::string impl_str("CUDA_DO_ALL_IMPL_InitializeGraph_" + 
                             (_graph.get_run_identifier()));
        galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME);
        StatTimer_cuda.start();
        InitializeGraph_cuda(*(allNodes.begin()), *(allNodes.end()),
                             infinity, src_node, cuda_ctx);
        StatTimer_cuda.stop();
      } else if (personality == CPU)
    #endif
    {
    galois::do_all(
      galois::iterate(allNodes.begin(), allNodes.end()),
      InitializeGraph{src_node, infinity, &_graph}, 
      galois::no_stats(), 
      galois::loopname(_graph.get_run_identifier("InitializeGraph").c_str()));
    }
  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
    sdata.dist_current = (graph->getGID(src) == local_src_node) ? 0 : local_infinity;
    sdata.dist_old = (graph->getGID(src) == local_src_node) ? 0 : local_infinity;
  }
};

struct FirstItr_SSSP {
  Graph * graph;
  FirstItr_SSSP(Graph* _graph):graph(_graph){}

  void static go(Graph& _graph) {
    uint32_t __begin, __end;
    if (_graph.isLocal(src_node)) {
      __begin = _graph.getLID(src_node);
      __end = __begin + 1;
    } else {
      __begin = 0;
      __end = 0;
    }
    _graph.set_num_iter(0);
#ifdef __GALOIS_HET_CUDA__
    if (personality == GPU_CUDA) {
      std::string impl_str("CUDA_DO_ALL_IMPL_SSSP_" + (_graph.get_run_identifier()));
      galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME);
      StatTimer_cuda.start();
      FirstItr_SSSP_cuda(__begin, __end, cuda_ctx);
      StatTimer_cuda.stop();
    } else if (personality == CPU)
#endif
    {
    // one node
    galois::do_all(
      galois::iterate(__begin, __end),
      FirstItr_SSSP{&_graph}, 
      galois::no_stats(), 
      galois::loopname(_graph.get_run_identifier("SSSP").c_str()));
    }

    _graph.sync<writeDestination, readSource, Reduce_min_dist_current, 
                Broadcast_dist_current, Bitset_dist_current>("SSSP");
    
    galois::runtime::reportStat_Tsum("SSSP", 
      "NUM_WORK_ITEMS_" + (_graph.get_run_identifier()), __end - __begin);
  }

  void operator()(GNode src) const {
    NodeData& snode = graph->getData(src);
    snode.dist_old = snode.dist_current;

    for (auto jj : graph->edges(src)) {
      GNode dst = graph->getEdgeDst(jj);
      auto& dnode = graph->getData(dst);
      uint32_t new_dist = graph->getEdgeData(jj) + snode.dist_current;
      uint32_t old_dist = galois::atomicMin(dnode.dist_current, new_dist);
      if (old_dist > new_dist) bitset_dist_current.set(dst);
    }
  }
};

struct SSSP {
  Graph* graph;
  galois::DGAccumulator<unsigned int>& DGAccumulator_accum;

  SSSP(Graph* _graph, galois::DGAccumulator<unsigned int>& _dga) : 
      graph(_graph), DGAccumulator_accum(_dga) {}

  void static go(Graph& _graph, galois::DGAccumulator<unsigned int>& dga) {
    using namespace galois::worklists;
    
    FirstItr_SSSP::go(_graph);
    
    unsigned _num_iterations = 1;
    
    const auto& nodesWithEdges = _graph.allNodesWithEdgesRange();

    do { 
      _graph.set_num_iter(_num_iterations);
      dga.reset();
      #ifdef __GALOIS_HET_CUDA__
      if (personality == GPU_CUDA) {
        std::string impl_str("CUDA_DO_ALL_IMPL_SSSP_" + (_graph.get_run_identifier()));
        galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME);
        StatTimer_cuda.start();
        int __retval = 0;
        SSSP_cuda(*nodesWithEdges.begin(), *nodesWithEdges.end(),
                  __retval, cuda_ctx);
        dga += __retval;
        StatTimer_cuda.stop();
      } else if (personality == CPU)
      #endif
      {
        galois::do_all(
          galois::iterate(nodesWithEdges),
          SSSP{ &_graph, dga },
          galois::no_stats(), 
          galois::loopname(_graph.get_run_identifier("SSSP").c_str()),
          galois::steal());
      }

      _graph.sync<writeDestination, readSource, Reduce_min_dist_current, 
                Broadcast_dist_current, Bitset_dist_current>("SSSP");
    
      galois::runtime::reportStat_Tsum("SSSP", 
        "NUM_WORK_ITEMS_" + (_graph.get_run_identifier()), 
        (unsigned long)dga.read_local());
      ++_num_iterations;
    } while ((_num_iterations < maxIterations) && dga.reduce(_graph.get_run_identifier()));

    if (galois::runtime::getSystemNetworkInterface().ID == 0) {
      galois::runtime::reportStat_Single("SSSP", 
        "NUM_ITERATIONS_" + std::to_string(_graph.get_run_num()), 
        (unsigned long)_num_iterations);
    }
  }

  void operator()(GNode src) const {
    NodeData& snode = graph->getData(src);

    if (snode.dist_old > snode.dist_current) {
      snode.dist_old = snode.dist_current;

      for (auto jj : graph->edges(src)) {
        GNode dst = graph->getEdgeDst(jj);
        auto& dnode = graph->getData(dst);
        uint32_t new_dist = graph->getEdgeData(jj) + snode.dist_current;
        uint32_t old_dist = galois::atomicMin(dnode.dist_current, new_dist);
        if (old_dist > new_dist) bitset_dist_current.set(dst);
      }

      DGAccumulator_accum+= 1;
    }
  }
};

/******************************************************************************/
/* Sanity check operators */
/******************************************************************************/

/* Prints total number of nodes visited + max distance */
struct SSSPSanityCheck {
  const uint32_t &local_infinity;
  Graph* graph;

  galois::DGAccumulator<uint64_t>& DGAccumulator_sum;
  galois::DGAccumulator<uint32_t>& DGAccumulator_max;
  galois::GReduceMax<uint32_t>& current_max;

  SSSPSanityCheck(const uint32_t& _infinity, Graph* _graph, 
                 galois::DGAccumulator<uint64_t>& dgas,
                 galois::DGAccumulator<uint32_t>& dgam,
                 galois::GReduceMax<uint32_t>& m) 
    : local_infinity(_infinity), graph(_graph), DGAccumulator_sum(dgas),
      DGAccumulator_max(dgam), current_max(m) {}

  void static go(Graph& _graph, galois::DGAccumulator<uint64_t>& dgas,
                 galois::DGAccumulator<uint32_t>& dgam,
                 galois::GReduceMax<uint32_t>& m) {
    dgas.reset();
    dgam.reset();

  #ifdef __GALOIS_HET_CUDA__
    if (personality == GPU_CUDA) {
      uint32_t sum, max;
      SSSPSanityCheck_cuda(sum, max, infinity, cuda_ctx);
      dgas += sum;
      dgam = max;
    }
    else
  #endif
    {
      m.reset();
      galois::do_all(galois::iterate(_graph.masterNodesRange().begin(), 
                                     _graph.masterNodesRange().end()),
                     SSSPSanityCheck(infinity, &_graph, dgas, dgam, m),
                     galois::no_stats(), galois::loopname("SSSPSanityCheck"));
      dgam = m.reduce();
    }

    uint64_t num_visited = dgas.reduce();
    uint32_t max_distance = dgam.reduce_max();

    // Only host 0 will print the info
    if (galois::runtime::getSystemNetworkInterface().ID == 0) {
      galois::gPrint("Number of nodes visited from source ", src_node, " is ", num_visited, "\n");
      galois::gPrint("Max distance from source ", src_node, " is ", max_distance, "\n");
    }
  }

  void operator()(GNode src) const {
    NodeData& src_data = graph->getData(src);

    if (src_data.dist_current < local_infinity) {
      DGAccumulator_sum += 1;
      current_max.update(src_data.dist_current);
    }
  }
};

/******************************************************************************/
/* Main */
/******************************************************************************/

constexpr static const char* const name = "SSSP - Distributed Heterogeneous "
                                          "with worklist.";
constexpr static const char* const desc = "Variant of Chaotic relaxation SSSP "
                                          "on Distributed Galois.";
constexpr static const char* const url = 0;

int main(int argc, char** argv) {
  galois::DistMemSys G;
  DistBenchStart(argc, argv, name, desc, url);

  auto& net = galois::runtime::getSystemNetworkInterface();

  if (net.ID == 0) {
    galois::runtime::reportParam("SSSP", "Max Iterations", 
      (unsigned long)maxIterations);
    galois::runtime::reportParam("SSSP", "Source Node ID", 
      (unsigned long)src_node);
  }

  galois::StatTimer StatTimer_total("TIMER_TOTAL", REGION_NAME);

  StatTimer_total.start();

  #ifdef __GALOIS_HET_CUDA__
  Graph* hg = distGraphInitialization<NodeData, unsigned int>(&cuda_ctx);
  #else
  Graph* hg = distGraphInitialization<NodeData, unsigned int>();
  #endif

  bitset_dist_current.resize(hg->size());

  galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");

  galois::StatTimer StatTimer_init("TIMER_GRAPH_INIT", REGION_NAME);

  StatTimer_init.start();
    InitializeGraph::go((*hg));
  StatTimer_init.stop();
  galois::runtime::getHostBarrier().wait();

  // accumulators for use in operators
  galois::DGAccumulator<unsigned int> DGAccumulator_accum;
  galois::DGAccumulator<uint64_t> DGAccumulator_sum;
  galois::DGAccumulator<uint32_t> DGAccumulator_max;
  galois::GReduceMax<uint32_t> m;

  for(auto run = 0; run < numRuns; ++run){
    galois::gPrint("[", net.ID, "] SSSP::go run ", run, " called\n");
    std::string timer_str("TIMER_" + std::to_string(run));
    galois::StatTimer StatTimer_main(timer_str.c_str(), REGION_NAME);

    StatTimer_main.start();
      SSSP::go(*hg, DGAccumulator_accum);
    StatTimer_main.stop();

    SSSPSanityCheck::go(*hg, DGAccumulator_sum, DGAccumulator_max, m);

    if ((run + 1) != numRuns) {
      #ifdef __GALOIS_HET_CUDA__
      if (personality == GPU_CUDA) { 
        bitset_dist_current_reset_cuda(cuda_ctx);
      } else
      #endif
      bitset_dist_current.reset();

      (*hg).set_num_run(run+1);
      InitializeGraph::go(*hg);
      galois::runtime::getHostBarrier().wait();
    }
  }

  StatTimer_total.stop();

  // Verify
  if (verify) {
    #ifdef __GALOIS_HET_CUDA__
    if (personality == CPU) { 
    #endif
      for (auto ii = (*hg).masterNodesRange().begin(); 
                ii != (*hg).masterNodesRange().end(); 
                ++ii) {
        galois::runtime::printOutput("% %\n", (*hg).getGID(*ii), 
                                     (*hg).getData(*ii).dist_current);
      }
    #ifdef __GALOIS_HET_CUDA__
    } else if(personality == GPU_CUDA)  {
      for (auto ii = (*hg).masterNodesRange().begin(); 
                ii != (*hg).masterNodesRange().end(); 
                ++ii) {
        galois::runtime::printOutput("% %\n", (*hg).getGID(*ii), 
                                     get_node_dist_current_cuda(cuda_ctx, *ii));
      }
    }
    #endif
  }

  return 0;
}
