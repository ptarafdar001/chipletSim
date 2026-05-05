// $Id$

/*
 Copyright (c) 2007-2015, Trustees of The Leland Stanford Junior University
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 Redistributions of source code must retain the above copyright notice, this 
 list of conditions and the following disclaimer.
 Redistributions in binary form must reproduce the above copyright notice, this
 list of conditions and the following disclaimer in the documentation and/or
 other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef _INJECTION_HPP_
#define _INJECTION_HPP_

#include <map>
#include <vector>
#include "config_utils.hpp"
#include "globals.hpp"   // GetSimTime()

using namespace std;

class InjectionProcess {
protected:
  int _nodes;
  double _rate;
  InjectionProcess(int nodes, double rate);
public:
  virtual ~InjectionProcess() {}
  virtual bool test(int source) = 0;
  virtual void reset();
  static InjectionProcess * New(string const & inject, int nodes, double load, 
				Configuration const * const config = NULL);
};

class BernoulliInjectionProcess : public InjectionProcess {
public:
  BernoulliInjectionProcess(int nodes, double rate);
  virtual bool test(int source);
};

class OnOffInjectionProcess : public InjectionProcess {
private:
  double _alpha;
  double _beta;
  double _r1;
  vector<int> _initial;
  vector<int> _state;
public:
  OnOffInjectionProcess(int nodes, double rate, double alpha, double beta, 
			double r1, vector<int> initial);
  virtual void reset();
  virtual bool test(int source);
};

// ---------------------------------------------------------------------------
// TraceInjectionProcess
// ---------------------------------------------------------------------------
// Paired with TraceTrafficPattern to replay a task-graph communication trace.
// test(source) returns true in cycle T for node N if the loaded trace has at
// least one unprocessed event with inject_cycle==T and src==N.
//
// Each (cycle, source) pair can have multiple events (multiple packets
// injected from the same node in the same cycle).  The process keeps a
// per-(cycle,source) counter so that successive calls to test() from the
// traffic-manager inner loop correctly consume events one at a time.
//
// The companion TraceTrafficPattern::dest() and size() must be called
// in the same cycle so they return the matching destination and flit count.
// ---------------------------------------------------------------------------
class TraceInjectionProcess : public InjectionProcess {
private:
  // All trace events sorted by (time, src)
  struct TraceEntry {
    int time;
    int src;
    int dst;
    int size;
  };
  vector<TraceEntry>        _entries;     // all events, sorted by time
  // (time, src) -> index of next unpopped entry in _entries
  // We use a scan-forward approach: _next_idx marks the first event whose
  // time >= current simulation time.
  size_t                    _scan_idx;
  // Remaining-count map for the current cycle: src -> # packets left to fire
  int                       _last_cycle;
  map<int, int>             _pending;     // src -> count for _last_cycle

  void _advance_to(int now);

public:
  // filename  : path to the trace file
  // nodes     : total node count (for validation)
  // trace_pat : must be the same TraceTrafficPattern so sizes are consistent
  TraceInjectionProcess(int nodes, const string & filename);
  virtual bool test(int source);
  virtual void reset();
  int  total_events() const { return (int)_entries.size(); }
  int  last_event_time() const {
    return _entries.empty() ? -1 : _entries.back().time;
  }
  // Return the dest / size for the *next* pending event for (now, source).
  // Called by TraceTrafficManager before consuming the event.
  int  peek_dest(int source) const;
  int  peek_size(int source) const;
};

#endif
