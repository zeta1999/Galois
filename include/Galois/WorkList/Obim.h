/** Scalable priority worklist -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2012, The University of Texas at Austin. All rights reserved.
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
 * @author Andrew Lenharth <andrewl@lenharth.org>
 */

#ifndef GALOIS_WORKLIST_OBIM_H
#define GALOIS_WORKLIST_OBIM_H

#include <map>

namespace Galois {
namespace WorkList {

template<class Indexer = DummyIndexer<int>, typename ContainerTy = FIFO<>, bool BSP=true, typename T = int, typename IndexTy = int, bool concurrent = true>
class OrderedByIntegerMetric : private boost::noncopyable {
  typedef typename ContainerTy::template rethread<concurrent> CTy;

  struct perItem {
    std::map<IndexTy, CTy*> local;
    IndexTy curIndex;
    IndexTy scanStart;
    CTy* current;
    unsigned int lastMasterVersion;

    perItem() :
      curIndex(std::numeric_limits<IndexTy>::min()), 
      scanStart(std::numeric_limits<IndexTy>::min()),
      current(0), lastMasterVersion(0) { }
  };

  std::deque<std::pair<IndexTy, CTy*> > masterLog;
  Runtime::LL::PaddedLock<concurrent> masterLock;
  volatile unsigned int masterVersion;

  Indexer I;

  Runtime::PerThreadStorage<perItem> current;

  void updateLocal_i(perItem& p) {
    for (; p.lastMasterVersion < masterVersion; ++p.lastMasterVersion) {
      std::pair<IndexTy, CTy*> logEntry = masterLog[p.lastMasterVersion];
      p.local[logEntry.first] = logEntry.second;
      assert(logEntry.second);
    }
  }

  bool updateLocal(perItem& p) {
    if (p.lastMasterVersion != masterVersion) {
      //masterLock.lock();
      updateLocal_i(p);
      //masterLock.unlock();
      return true;
    }
    return false;
  }

  CTy* updateLocalOrCreate(perItem& p, IndexTy i) {
    //Try local then try update then find again or else create and update the master log
    CTy*& lC = p.local[i];
    if (lC)
      return lC;
    //update local until we find it or we get the write lock
    do {
      updateLocal(p);
      if (lC)
	return lC;
    } while (!masterLock.try_lock());
    //we have the write lock, update again then create
    updateLocal(p);
    if (!lC) {
      lC = new CTy();
      p.lastMasterVersion = masterVersion + 1;
      masterLog.push_back(std::make_pair(i, lC));
      __sync_fetch_and_add(&masterVersion, 1);
      p.local[i] = lC;
    }
    masterLock.unlock();
    return lC;
  }

 public:
  template<bool newconcurrent>
    using rethread = OrderedByIntegerMetric<Indexer,ContainerTy,BSP,T,IndexTy,newconcurrent>;
  template<typename Tnew>
    using retype = OrderedByIntegerMetric<Indexer,typename ContainerTy::template retype<Tnew>,BSP,Tnew,decltype(Indexer()(Tnew())),concurrent>;

  typedef T value_type;

  OrderedByIntegerMetric(const Indexer& x = Indexer())
    :masterVersion(0), I(x)
  { }

  ~OrderedByIntegerMetric() {
    for (typename std::deque<std::pair<IndexTy, CTy*> >::iterator ii = masterLog.begin(), ee = masterLog.end(); ii != ee; ++ii) {
      delete ii->second;
    }
  }

  void push(const value_type& val) {
    IndexTy index = I(val);
    perItem& p = *current.getLocal();
    //fastpath
    if (index == p.curIndex && p.current) {
      p.current->push(val);
      return;
    }

    //slow path
    CTy* lC = updateLocalOrCreate(p, index);
    if (BSP && index < p.scanStart)
      p.scanStart = index;
    //opportunistically move to higher priority work
    if (index < p.curIndex) {
      p.curIndex = index;
      p.current = lC;
    }
    lC->push(val);
  }

  template<typename Iter>
  void push(Iter b, Iter e) {
    while (b != e)
      push(*b++);
  }

  template<typename RangeTy>
  void push_initial(RangeTy range) {
    push(range.local_begin(), range.local_end());
  }

  boost::optional<value_type> pop() {
    //Find a successful pop
    perItem& p = *current.getLocal();
    CTy*& C = p.current;
    boost::optional<value_type> retval;
    if (C && (retval = C->pop()))
      return retval;
    //Failed, find minimum bin
    updateLocal(p);
    unsigned myID = Runtime::LL::getTID();
    bool localLeader = Runtime::LL::isPackageLeaderForSelf(myID);

    IndexTy msS = std::numeric_limits<IndexTy>::min();
    if (BSP) {
      msS = p.scanStart;
      if (localLeader)
	for (unsigned i = 0; i <  Runtime::activeThreads; ++i)
	  msS = std::min(msS, current.getRemote(i)->scanStart);
      else
	msS = std::min(msS, current.getRemote(Runtime::LL::getLeaderForThread(myID))->scanStart);
    }

    for (typename std::map<IndexTy, CTy*>::iterator ii = p.local.lower_bound(msS),
        ee = p.local.end(); ii != ee; ++ii) {
      if ((retval = ii->second->pop())) {
	C = ii->second;
	p.curIndex = ii->first;
	p.scanStart = ii->first;
	return retval;
      }
    }
    return boost::optional<value_type>();
  }
};
GALOIS_WLCOMPILECHECK(OrderedByIntegerMetric)

} // end namespace WorkList
} // end namespace Galois

#endif
