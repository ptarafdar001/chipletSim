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

#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <algorithm>
#include <cassert>
#include <limits>
#include "random_utils.hpp"
#include "injection.hpp"

using namespace std;

InjectionProcess::InjectionProcess(int nodes, double rate)
  : _nodes(nodes), _rate(rate)
{
  if(nodes <= 0) {
    cout << "Error: Number of nodes must be greater than zero." << endl;
    exit(-1);
  }
  if((rate < 0.0) || (rate > 1.0)) {
    cout << "Error: Injection process must have load between 0.0 and 1.0."
	 << endl;
    exit(-1);
  }
}

void InjectionProcess::reset()
{

}

InjectionProcess * InjectionProcess::New(string const & inject, int nodes, 
					 double load, 
					 Configuration const * const config)
{
  string process_name;
  string param_str;
  size_t left = inject.find_first_of('(');
  if(left == string::npos) {
    process_name = inject;
  } else {
    process_name = inject.substr(0, left);
    size_t right = inject.find_last_of(')');
    if(right == string::npos) {
      param_str = inject.substr(left+1);
    } else {
      param_str = inject.substr(left+1, right-left-1);
    }
  }
  vector<string> params = tokenize_str(param_str);

  InjectionProcess * result = NULL;
  if(process_name == "bernoulli") {
    result = new BernoulliInjectionProcess(nodes, load);
  } else if(process_name == "on_off") {
    bool missing_params = false;
    double alpha = numeric_limits<double>::quiet_NaN();
    if(params.size() < 1) {
      if(config) {
	alpha = config->GetFloat("burst_alpha");
      } else {
	missing_params = true;
      }
    } else {
      alpha = atof(params[0].c_str());
    }
    double beta = numeric_limits<double>::quiet_NaN();
    if(params.size() < 2) {
      if(config) {
	beta = config->GetFloat("burst_beta");
      } else {
	missing_params = true;
      }
    } else {
      beta = atof(params[1].c_str());
    }
    double r1 = numeric_limits<double>::quiet_NaN();
    if(params.size() < 3) {
      r1 = config ? config->GetFloat("burst_r1") : -1.0;
    } else {
      r1 = atof(params[2].c_str());
    }
    if(missing_params) {
      cout << "Missing parameters for injection process: " << inject << endl;
      exit(-1);
    }
    if((alpha < 0.0 && beta < 0.0) || 
       (alpha < 0.0 && r1 < 0.0) || 
       (beta < 0.0 && r1 < 0.0) || 
       (alpha >= 0.0 && beta >= 0.0 && r1 >= 0.0)) {
      cout << "Invalid parameters for injection process: " << inject << endl;
      exit(-1);
    }
    vector<int> initial(nodes);
    if(params.size() > 3) {
      initial = tokenize_int(params[2]);
      initial.resize(nodes, initial.back());
    } else {
      for(int n = 0; n < nodes; ++n) {
	initial[n] = RandomInt(1);
      }
    }
    result = new OnOffInjectionProcess(nodes, load, alpha, beta, r1, initial);
  } else if(process_name == "trace") {
    // Trace-driven injection: fires exactly when the trace says so.
    // Requires trace_file to be set in config (same file used by TraceTrafficPattern).
    if(!config) {
      cout << "Error: trace injection process requires a configuration object." << endl;
      exit(-1);
    }
    string filename = config->GetStr("trace_file");
    if(filename.empty()) {
      cout << "Error: trace injection_process requires trace_file to be set in config." << endl;
      exit(-1);
    }
    result = new TraceInjectionProcess(nodes, filename);
  } else {
    cout << "Invalid injection process: " << inject << endl;
    exit(-1);
  }
  return result;
}

//=============================================================

BernoulliInjectionProcess::BernoulliInjectionProcess(int nodes, double rate)
  : InjectionProcess(nodes, rate)
{

}

bool BernoulliInjectionProcess::test(int source)
{
  assert((source >= 0) && (source < _nodes));
  return (RandomFloat() < _rate);
}

//=============================================================

OnOffInjectionProcess::OnOffInjectionProcess(int nodes, double rate, 
					     double alpha, double beta, 
					     double r1, vector<int> initial)
  : InjectionProcess(nodes, rate), 
    _alpha(alpha), _beta(beta), _r1(r1), _initial(initial)
{
  assert(alpha <= 1.0);
  assert(beta <= 1.0);
  assert(r1 <= 1.0);
  if(alpha < 0.0) {
    assert(beta >= 0.0);
    assert(r1 >= 0.0);
    _alpha = beta * rate / (r1 - rate);
  } else if(beta < 0.0) {
    assert(alpha >= 0.0);
    assert(r1 >= 0.0);
    _beta = alpha * (r1 - rate) / rate;
  } else {
    assert(r1 < 0.0);
    _r1 = rate * (alpha + beta) / alpha;
  }
  reset();
}

void OnOffInjectionProcess::reset()
{
  _state = _initial;
}

bool OnOffInjectionProcess::test(int source)
{
  assert((source >= 0) && (source < _nodes));

  // advance state
  _state[source] = 
    _state[source] ? (RandomFloat() >= _beta) : (RandomFloat() < _alpha);

  // generate packet
  return _state[source] && (RandomFloat() < _r1);
}

// ==========================================================================
// TraceInjectionProcess implementation
// ==========================================================================
//
// Design notes:
//
//  * The trace file has lines:  inject_cycle  src  dst  size_flits
//  * Events are sorted by (time, src) on load.
//  * _scan_idx points to the first entry with time >= _last_cycle so we
//    skip stale events in O(1) amortized.
//  * _pending[src] = number of packets from node `src` still to be fired
//    in the current cycle.  It is rebuilt once per cycle on demand.
//  * test(source) decrements _pending[source] and returns true while > 0.
//    The paired TraceTrafficPattern::dest/size is called by the traffic
//    manager in the same cycle to get destination and flit count.
//
TraceInjectionProcess::TraceInjectionProcess(int nodes,
                                             const string & filename)
  : InjectionProcess(nodes, 0.0),   // rate unused in trace mode
    _scan_idx(0), _last_cycle(-1)
{
  ifstream fin(filename.c_str());
  if(!fin.is_open()) {
    cout << "TraceInjectionProcess: Cannot open trace file: "
         << filename << endl;
    exit(-1);
  }

  int t, s, d, sz, line = 0;
  while(fin >> t >> s >> d >> sz) {
    ++line;
    if(s < 0 || s >= nodes) {
      cout << "TraceInjectionProcess: line " << line
           << ": src " << s << " out of range [0," << nodes-1 << "]" << endl;
      exit(-1);
    }
    if(d < 0 || d >= nodes) {
      cout << "TraceInjectionProcess: line " << line
           << ": dst " << d << " out of range [0," << nodes-1 << "]" << endl;
      exit(-1);
    }
    if(sz < 1) sz = 1;
    TraceEntry e; e.time = t; e.src = s; e.dst = d; e.size = sz;
    _entries.push_back(e);
  }
  fin.close();

  sort(_entries.begin(), _entries.end(),
       [](const TraceEntry & a, const TraceEntry & b){
         return a.time < b.time || (a.time == b.time && a.src < b.src);
       });

  cout << "TraceInjectionProcess: loaded " << _entries.size()
       << " events from " << filename << endl;
  if(!_entries.empty())
    cout << "  Cycle range: " << _entries.front().time
         << " to " << _entries.back().time << endl;
}

void TraceInjectionProcess::reset()
{
  _scan_idx  = 0;
  _last_cycle = -1;
  _pending.clear();
}

void TraceInjectionProcess::_advance_to(int now)
{
  if(now == _last_cycle) return;   // already built for this cycle

  _last_cycle = now;
  _pending.clear();

  // Advance _scan_idx past events with time < now
  while(_scan_idx < _entries.size() && _entries[_scan_idx].time < now)
    ++_scan_idx;

  // Count events with time == now
  for(size_t i = _scan_idx; i < _entries.size() && _entries[i].time == now; ++i)
    _pending[_entries[i].src]++;
}

bool TraceInjectionProcess::test(int source)
{
  assert(source >= 0 && source < _nodes);
  int now = GetSimTime();
  _advance_to(now);
  map<int,int>::iterator it = _pending.find(source);
  if(it != _pending.end() && it->second > 0) {
    it->second--;    // consume one event slot
    return true;
  }
  return false;
}

int TraceInjectionProcess::peek_dest(int source) const
{
  int now = GetSimTime();
  for(size_t i = _scan_idx; i < _entries.size() && _entries[i].time == now; ++i)
    if(_entries[i].src == source) return _entries[i].dst;
  return source;  // fallback: self
}

int TraceInjectionProcess::peek_size(int source) const
{
  int now = GetSimTime();
  for(size_t i = _scan_idx; i < _entries.size() && _entries[i].time == now; ++i)
    if(_entries[i].src == source) return _entries[i].size;
  return 1;
}
