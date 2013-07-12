/** GMetis -*- C++ -*-
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
 * @author Xin Sui <xinsui@cs.utexas.edu>
 * @author Nikunj Yadav <nikunj@cs.utexas.edu>
 * @author Andrew Lenharth <andrew@lenharth.org>
 */

#include "Metis.h"

#include <set>

namespace {

struct gainIndexer {
  static GGraph* g;

  int operator()(GNode n) {
    int retval = 0;
    Galois::MethodFlag flag = Galois::NONE;
    unsigned nPart = g->getData(n, flag).getPart();
    for (auto ii = g->edge_begin(n, flag), ee = g->edge_end(n); ii != ee; ++ii) {
      GNode neigh = g->getEdgeDst(ii, flag);
      if (g->getData(neigh, flag).getPart() == nPart)
        retval -= g->getEdgeData(ii, flag);
      else
        retval += g->getEdgeData(ii, flag);
    }
    return -retval / 16;
  }
};

GGraph* gainIndexer::g;

bool isBoundary(GGraph& g, GNode n) {
  unsigned nPart = g.getData(n).getPart();
  for (auto ii = g.edge_begin(n), ee =g.edge_end(n); ii != ee; ++ii)
    if (g.getData(g.getEdgeDst(ii)).getPart() != nPart)
      return true;
  return false;
}

struct findBoundary {
  Galois::InsertBag<GNode>& b;
  GGraph& g;
  findBoundary(Galois::InsertBag<GNode>& _b, GGraph& _g) :b(_b), g(_g) {}
  void operator()(GNode n) {
    if (isBoundary(g, n))
      b.push(n);
  }
};

struct findBoundaryAndProject {
  Galois::InsertBag<GNode>& b;
  GGraph& cg;
  GGraph& fg;
  findBoundaryAndProject(Galois::InsertBag<GNode>& _b, GGraph& _cg, GGraph& _fg) :b(_b), cg(_cg), fg(_fg) {}
  void operator()(GNode n) {
    if (isBoundary(cg, n))
      b.push(n);
    auto& cn = cg.getData(n, Galois::MethodFlag::NONE);
    unsigned part = cn.getPart();
    for (unsigned x = 0; x < cn.numChildren(); ++x)
      fg.getData(cn.getChild(x), Galois::MethodFlag::NONE).setPart(part);
  }
};

template<bool balance>
struct refine_BKL2 {
  unsigned maxSize;
  GGraph& cg;
  GGraph* fg;
  std::vector<partInfo>& parts;

  typedef int tt_needs_per_iter_alloc;

  refine_BKL2(unsigned ms, GGraph& _cg, GGraph* _fg, std::vector<partInfo>& _p) : maxSize(ms), cg(_cg), fg(_fg), parts(_p) {}

  //Find the partition n is most connected to
  template<typename Context>
  unsigned pickPartitionEC(GNode n, Context& cnx) {
    std::vector<unsigned, Galois::PerIterAllocTy::rebind<unsigned>::other> edges(parts.size(), 0, cnx.getPerIterAlloc());
    unsigned P = cg.getData(n).getPart();
    for (auto ii = cg.edge_begin(n), ee = cg.edge_end(n); ii != ee; ++ii) {
      GNode neigh = cg.getEdgeDst(ii);
      auto& nd = cg.getData(neigh);
      if (parts[nd.getPart()].partWeight < maxSize
          || nd.getPart() == P)
        edges[nd.getPart()] += cg.getEdgeData(ii);
    }
    return std::distance(edges.begin(), std::max_element(edges.begin(), edges.end()));
  }

  //Find the smallest partition n is connected to
  template<typename Context>
  unsigned pickPartitionMP(GNode n, Context& cnx) {
    unsigned P = cg.getData(n).getPart();
    unsigned W = parts[P].partWeight;
    std::vector<unsigned, Galois::PerIterAllocTy::rebind<unsigned>::other> edges(parts.size(), ~0, cnx.getPerIterAlloc());
     edges[P] = W;
    W = (double)W * 0.9;
    for (auto ii = cg.edge_begin(n), ee = cg.edge_end(n); ii != ee; ++ii) {
      GNode neigh = cg.getEdgeDst(ii);
      auto& nd = cg.getData(neigh);
      if (parts[nd.getPart()].partWeight < W)
        edges[nd.getPart()] = parts[nd.getPart()].partWeight;
    }
    return std::distance(edges.begin(), std::min_element(edges.begin(), edges.end()));
  }


  template<typename Context>
  void operator()(GNode n, Context& cnx) {
    auto& nd = cg.getData(n);
    unsigned curpart = nd.getPart();
    unsigned newpart = balance ? pickPartitionMP(n, cnx) : pickPartitionEC(n, cnx);
    if (curpart != newpart) {
      nd.setPart(newpart);
      //__sync_fetch_and_sub(&maxSize, 1);
      __sync_fetch_and_sub(&parts[curpart].partWeight, nd.getWeight());
      __sync_fetch_and_add(&parts[newpart].partWeight, nd.getWeight());
      for (auto ii = cg.edge_begin(n), ee = cg.edge_end(n); ii != ee; ++ii) {
        GNode neigh = cg.getEdgeDst(ii);
        auto& ned = cg.getData(neigh);
        //if (ned.getPart() != newpart)
        cnx.push(neigh);
      }
      if (fg)
        for (unsigned x = 0; x < nd.numChildren(); ++x)
          fg->getData(nd.getChild(x), Galois::MethodFlag::NONE).setPart(newpart);
    }
  }

  static void go(unsigned ms, GGraph& cg, GGraph* fg, std::vector<partInfo>& p) {
    typedef Galois::WorkList::dChunkedFIFO<8> Chunk;
    typedef Galois::WorkList::OrderedByIntegerMetric<gainIndexer, Chunk, 10> pG;
    gainIndexer::g = &cg;
    Galois::InsertBag<GNode> boundary;
    if (fg)
      Galois::do_all_local(cg, findBoundaryAndProject(boundary, cg, *fg), "boundary");
    else
      Galois::do_all_local(cg, findBoundary(boundary, cg), "boundary");
    Galois::for_each_local<pG>(boundary, refine_BKL2(ms, cg, fg, p), "refine");
  }
};

struct projectPart {
  GGraph* fineGraph;
  GGraph* coarseGraph;
  std::vector<partInfo>& parts;

  projectPart(MetisGraph* Graph, std::vector<partInfo>& p) :fineGraph(Graph->getFinerGraph()->getGraph()), coarseGraph(Graph->getGraph()), parts(p) {}

  void operator()(GNode n) {
    auto& cn = coarseGraph->getData(n);
    unsigned part = cn.getPart();
    for (unsigned x = 0; x < cn.numChildren(); ++x)
      fineGraph->getData(cn.getChild(x)).setPart(part);
  }

  static void go(MetisGraph* Graph, std::vector<partInfo>& p) {
    Galois::do_all_local(*Graph->getGraph(), projectPart(Graph, p), "project");
  }
};

} //anon namespace


int gain(GGraph& g, GNode n) {
  int retval = 0;
  unsigned nPart = g.getData(n).getPart();
  for (auto ii = g.edge_begin(n), ee =g.edge_end(n); ii != ee; ++ii) {
    GNode neigh = g.getEdgeDst(ii);
    if (g.getData(neigh).getPart() == nPart)
      retval -= g.getEdgeData(ii);
    else
      retval += g.getEdgeData(ii);
  }
  return retval;
}

struct parallelBoundary {
  GGraph& g;
  Galois::InsertBag<GNode> &bag;
  parallelBoundary(Galois::InsertBag<GNode> &bag, GGraph& graph):bag(bag),g(graph) {

  }
  void operator()(GNode n,Galois::UserContext<GNode>&ctx) {
      if (gain(g,n) > 0)  
        bag.push(n);
  }
};
void refineOneByOne(GGraph& g, std::vector<partInfo>& parts) {
  std::vector<GNode>  boundary;
  int meanWeight =0;
  for (int i =0; i<parts.size(); i++)
    meanWeight += parts[i].partWeight;
  meanWeight /= parts.size();
  Galois::InsertBag<GNode> boundaryBag;
  parallelBoundary pB(boundaryBag, g);
  Galois::for_each(g.begin(), g.end(), pB, "Get Boundary" );

  for (auto ii = boundaryBag.begin(), ie =boundaryBag.end(); ii!=ie;ii++){
      GNode n = (*ii) ;
      unsigned nPart = g.getData(n).getPart();
      int part[parts.size()];
      for (int i =0; i<parts.size(); i++)part[i]=0;
      for (auto ii = g.edge_begin(n), ee = g.edge_end(n); ii != ee; ++ii) {
        GNode neigh = g.getEdgeDst(ii);
        part[g.getData(neigh).getPart()]+=g.getEdgeData(ii);
      }
      int t = part[nPart];
      int p = nPart;
      for (int i =0; i<parts.size(); i++)
        if (i!=nPart && part[i] > t && parts[nPart].partWeight>  parts[i].partWeight*(98)/(100) && parts[nPart].partWeight > meanWeight*98/100){
          t = part[i];
          p = i;
        }
    if(p != nPart){ 
      g.getData(n).setPart(p);
      parts[p].partWeight += g.getData(n).getWeight();
      parts[nPart].partWeight -= g.getData(n).getWeight();
    }
  }
}

void refine_BKL(GGraph& g, std::vector<partInfo>& parts) {
  std::set<GNode> boundary;
  
  //find boundary nodes with positive gain
  Galois::InsertBag<GNode> boundaryBag;
  parallelBoundary pB(boundaryBag, g);
  Galois::for_each(g.begin(), g.end(), pB, "Get Boundary" );
  for (auto ii = boundaryBag.begin(), ie =boundaryBag.end(); ii!=ie;ii++ ){
    boundary.insert(*ii);}

  //refine by swapping with a neighbor high-gain node
  while (!boundary.empty()) {
    GNode n = *boundary.begin();
    boundary.erase(boundary.begin());
    unsigned nPart = g.getData(n).getPart();
    for (auto ii = g.edge_begin(n), ee = g.edge_end(n); ii != ee; ++ii) {
      GNode neigh = g.getEdgeDst(ii);
      unsigned neighPart = g.getData(neigh).getPart();
      if (neighPart != nPart && boundary.count(neigh) &&
          gain(g, n) > 0 && gain(g, neigh) > 0 ) {
        unsigned nWeight = g.getData(n).getWeight();
        unsigned neighWeight = g.getData(neigh).getWeight();
        //swap
        g.getData(n).setPart(neighPart);
        g.getData(neigh).setPart(nPart);
        //update partinfo
        parts[neighPart].partWeight += nWeight;
        parts[neighPart].partWeight -= neighWeight;
        parts[nPart].partWeight += neighWeight;
        parts[nPart].partWeight -= nWeight;
        //remove nodes
        boundary.erase(neigh);
        break;
      }
    }
  }
}


void refine(MetisGraph* coarseGraph, std::vector<partInfo>& parts, unsigned maxSize, refinementMode refM) {

  do {
  
    MetisGraph* fineGraph = coarseGraph->getFinerGraph();
    bool doProject = true;
    //refine nparts times
    switch (refM) {
    case BKL2:refine_BKL2<false>::go(maxSize, *coarseGraph->getGraph(), fineGraph ? fineGraph->getGraph() : nullptr, parts); doProject = false; break;
    case BKL: refine_BKL(*coarseGraph->getGraph(), parts); break;
    case ROBO: refineOneByOne(*coarseGraph->getGraph(), parts); break;
    default: abort();
    }
    // std::cout << "Refinement of " << coarseGraph->getGraph() << "\n";
    // printPartStats(parts);

    //project up
    if (fineGraph && doProject) {
      projectPart::go(coarseGraph, parts);
    }
  } while ((coarseGraph = coarseGraph->getFinerGraph()));
}

void balance(MetisGraph* coarseGraph, std::vector<partInfo>& parts, unsigned maxSize) {
    MetisGraph* fineGraph = coarseGraph->getFinerGraph();
    refine_BKL2<true>::go(maxSize, *coarseGraph->getGraph(), fineGraph ? fineGraph->getGraph() : nullptr, parts);
}
