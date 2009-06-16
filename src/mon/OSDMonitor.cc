// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include "OSDMonitor.h"
#include "Monitor.h"
#include "MDSMonitor.h"
#include "PGMonitor.h"

#include "MonitorStore.h"

#include "crush/CrushWrapper.h"

#include "messages/MOSDFailure.h"
#include "messages/MOSDMap.h"
#include "messages/MOSDGetMap.h"
#include "messages/MOSDBoot.h"
#include "messages/MOSDAlive.h"
#include "messages/MPoolSnap.h"
#include "messages/MPoolSnapReply.h"
#include "messages/MMonCommand.h"
#include "messages/MRemoveSnaps.h"
#include "messages/MOSDScrub.h"

#include "common/Timer.h"

#include "config.h"

#include <sstream>

#define DOUT_SUBSYS mon
#undef dout_prefix
#define dout_prefix _prefix(mon, osdmap)
static ostream& _prefix(Monitor *mon, OSDMap& osdmap) {
  return *_dout << dbeginl 
		<< "mon" << mon->whoami
		<< (mon->is_starting() ? (const char*)"(starting)":(mon->is_leader() ? (const char*)"(leader)":(mon->is_peon() ? (const char*)"(peon)":(const char*)"(?\?)")))
		<< ".osd e" << osdmap.get_epoch() << " ";
}


// FAKING

class C_Mon_FakeOSDFailure : public Context {
  OSDMonitor *mon;
  int osd;
  bool down;
public:
  C_Mon_FakeOSDFailure(OSDMonitor *m, int o, bool d) : mon(m), osd(o), down(d) {}
  void finish(int r) {
    mon->fake_osd_failure(osd,down);
  }
};

void OSDMonitor::fake_osd_failure(int osd, bool down) 
{
  if (down) {
    dout(1) << "fake_osd_failure DOWN osd" << osd << dendl;
    pending_inc.new_down[osd] = false;
  } else {
    dout(1) << "fake_osd_failure OUT osd" << osd << dendl;
    pending_inc.new_weight[osd] = CEPH_OSD_OUT;
  }
  propose_pending();

  // fixme
  //bcast_latest_osd();
  //bcast_latest_mds();
}

void OSDMonitor::fake_osdmap_update()
{
  dout(1) << "fake_osdmap_update" << dendl;
  propose_pending();

  // tell a random osd
  int osd = rand() % g_conf.num_osd;
  send_latest(osdmap.get_inst(osd));
}


void OSDMonitor::fake_reorg() 
{
  int r = rand() % g_conf.num_osd;
  
  if (osdmap.is_out(r)) {
    dout(1) << "fake_reorg marking osd" << r << " in" << dendl;
    pending_inc.new_weight[r] = CEPH_OSD_IN;
  } else {
    dout(1) << "fake_reorg marking osd" << r << " out" << dendl;
    pending_inc.new_weight[r] = CEPH_OSD_OUT;
  }

  propose_pending();
  send_latest(osdmap.get_inst(r));  // after
}


/************ MAPS ****************/


void OSDMonitor::create_initial(bufferlist& bl)
{
  dout(10) << "create_initial for " << mon->monmap->fsid << dendl;

  OSDMap newmap;
  newmap.decode(bl);
  newmap.set_epoch(1);
  newmap.set_fsid(mon->monmap->fsid);
  newmap.created = newmap.modified = g_clock.now();

  // encode into pending incremental
  newmap.encode(pending_inc.fullmap);
}

bool OSDMonitor::update_from_paxos()
{
  assert(paxos->is_active());

  version_t paxosv = paxos->get_version();
  if (paxosv == osdmap.epoch) return true;
  assert(paxosv >= osdmap.epoch);

  dout(15) << "update_from_paxos paxos e " << paxosv 
	   << ", my e " << osdmap.epoch << dendl;

  if (osdmap.epoch == 0 && paxosv > 1) {
    // startup: just load latest full map
    bufferlist latest;
    version_t v = paxos->get_latest(latest);
    if (v) {
      dout(7) << "update_from_paxos startup: loading latest full map e" << v << dendl;
      osdmap.decode(latest);
    }
  } 
  
  // walk through incrementals
  bufferlist bl;
  while (paxosv > osdmap.epoch) {
    bool success = paxos->read(osdmap.epoch+1, bl);
    assert(success);
    
    dout(7) << "update_from_paxos  applying incremental " << osdmap.epoch+1 << dendl;
    OSDMap::Incremental inc(bl);
    osdmap.apply_incremental(inc);

    // write out the full map for all past epochs
    bl.clear();
    osdmap.encode(bl);
    mon->store->put_bl_sn(bl, "osdmap_full", osdmap.epoch);

    // share
    dout(1) << osdmap << dendl;
  }

  // save latest
  paxos->stash_latest(paxosv, bl);

  // populate down -> out map
  for (int o = 0; o < osdmap.get_max_osd(); o++)
    if (osdmap.is_down(o) && osdmap.is_in(o) &&
	down_pending_out.count(o) == 0) {
      dout(10) << " adding osd" << o << " to down_pending_out map" << dendl;
      down_pending_out[o] = g_clock.now();
    }

  if (mon->is_leader()) {
    // kick pgmon, make sure it's seen the latest map
    mon->pgmon()->check_osd_map(osdmap.epoch);

    bcast_latest_mds();
  }

  send_to_waiting();
    
  return true;
}


void OSDMonitor::create_pending()
{
  pending_inc = OSDMap::Incremental(osdmap.epoch+1);
  pending_inc.fsid = mon->monmap->fsid;
  
  dout(10) << "create_pending e " << pending_inc.epoch << dendl;
}


void OSDMonitor::encode_pending(bufferlist &bl)
{
  dout(10) << "encode_pending e " << pending_inc.epoch
	   << dendl;
  
  // finalize up pending_inc
  pending_inc.modified = g_clock.now();

  // tell me about it
  for (map<int32_t,uint8_t>::iterator i = pending_inc.new_down.begin();
       i != pending_inc.new_down.end();
       i++) {
    dout(2) << " osd" << i->first << " DOWN clean=" << (int)i->second << dendl;
    // no: this screws up map delivery on shutdown
    //mon->messenger->mark_down(osdmap.get_addr(i->first));
  }
  for (map<int32_t,entity_addr_t>::iterator i = pending_inc.new_up.begin();
       i != pending_inc.new_up.end(); 
       i++) { 
    dout(2) << " osd" << i->first << " UP " << i->second << dendl;
  }
  for (map<int32_t,uint32_t>::iterator i = pending_inc.new_weight.begin();
       i != pending_inc.new_weight.end();
       i++) {
    if (i->second == CEPH_OSD_OUT) {
      dout(2) << " osd" << i->first << " OUT" << dendl;
    } else if (i->second == CEPH_OSD_IN) {
      dout(2) << " osd" << i->first << " IN" << dendl;
    } else {
      dout(2) << " osd" << i->first << " WEIGHT " << hex << i->second << dec << dendl;
    }
  }

  // encode
  assert(paxos->get_version() + 1 == pending_inc.epoch);
  pending_inc.encode(bl);
}


void OSDMonitor::committed()
{
  // tell any osd
  int r = osdmap.get_any_up_osd();
  if (r >= 0) {
    dout(10) << "committed, telling random osd" << r << " all about it" << dendl;
    send_latest(osdmap.get_inst(r), osdmap.get_epoch() - 1);  // whatev, they'll request more if they need it
  }
}


// -------------

bool OSDMonitor::preprocess_query(Message *m)
{
  dout(10) << "preprocess_query " << *m << " from " << m->get_orig_source_inst() << dendl;

  switch (m->get_type()) {
    // READs
  case CEPH_MSG_OSD_GETMAP:
    handle_osd_getmap((MOSDGetMap*)m);
    return true;
    
  case MSG_MON_COMMAND:
    return preprocess_command((MMonCommand*)m);

    // damp updates
  case MSG_OSD_FAILURE:
    return preprocess_failure((MOSDFailure*)m);
  case MSG_OSD_BOOT:
    return preprocess_boot((MOSDBoot*)m);
  case MSG_OSD_ALIVE:
    return preprocess_alive((MOSDAlive*)m);
    /*
  case MSG_OSD_OUT:
    return preprocess_out((MOSDOut*)m);
    */

  case MSG_POOLSNAP:
    return preprocess_pool_snap((MPoolSnap*)m);

  case MSG_REMOVE_SNAPS:
    return preprocess_remove_snaps((MRemoveSnaps*)m);
    
  default:
    assert(0);
    delete m;
    return true;
  }
}

bool OSDMonitor::prepare_update(Message *m)
{
  dout(7) << "prepare_update " << *m << " from " << m->get_orig_source_inst() << dendl;
  
  switch (m->get_type()) {
    // damp updates
  case MSG_OSD_FAILURE:
    return prepare_failure((MOSDFailure*)m);
  case MSG_OSD_BOOT:
    return prepare_boot((MOSDBoot*)m);
  case MSG_OSD_ALIVE:
    return prepare_alive((MOSDAlive*)m);

  case MSG_MON_COMMAND:
    return prepare_command((MMonCommand*)m);
    
    /*
  case MSG_OSD_OUT:
    return prepare_out((MOSDOut*)m);
    */
  case MSG_POOLSNAP:
    return prepare_pool_snap((MPoolSnap*)m);

  case MSG_REMOVE_SNAPS:
    return prepare_remove_snaps((MRemoveSnaps*)m);

  default:
    assert(0);
    delete m;
  }

  return false;
}

bool OSDMonitor::should_propose(double& delay)
{
  dout(10) << "should_propose" << dendl;

  // if full map, propose immediately!  any subsequent changes will be clobbered.
  if (pending_inc.fullmap.length())
    return true;

  // adjust osd weights?
  if (osd_weight.size() == (unsigned)osdmap.get_max_osd()) {
    dout(0) << " adjusting osd weights based on " << osd_weight << dendl;
    osdmap.adjust_osd_weights(osd_weight, pending_inc);
    delay = 0.0;
    osd_weight.clear();
    return true;
  }

  return PaxosService::should_propose(delay);
}



// ---------------------------
// READs

void OSDMonitor::handle_osd_getmap(MOSDGetMap *m)
{
  dout(7) << "handle_osd_getmap from " << m->get_orig_source()
	  << " start " << m->get_start_epoch()
	  << dendl;
  
  if (ceph_fsid_compare(&m->fsid, &mon->monmap->fsid)) {
    dout(0) << "handle_osd_getmap on fsid " << m->fsid << " != " << mon->monmap->fsid << dendl;
    goto out;
  }

  if (m->get_start_epoch()) {
    if (m->get_start_epoch() <= osdmap.get_epoch())
	send_incremental(m->get_orig_source_inst(), m->get_start_epoch());
    else
      waiting_for_map[m->get_orig_source_inst()] = m->get_start_epoch();
  } else
    send_full(m->get_orig_source_inst());
  
 out:
  delete m;
}

// ---------------------------
// UPDATEs

// failure --

bool OSDMonitor::preprocess_failure(MOSDFailure *m)
{
  // who is failed
  int badboy = m->get_failed().name.num();

  if (ceph_fsid_compare(&m->fsid, &mon->monmap->fsid)) {
    dout(0) << "preprocess_failure on fsid " << m->fsid << " != " << mon->monmap->fsid << dendl;
    goto didit;
  }

  /*
   * FIXME
   * this whole thing needs a rework of some sort.  we shouldn't
   * be taking any failure report on faith.  if A and B can't talk
   * to each other either A or B should be killed, but we should
   * make some attempt to make sure we choose the right one.
   */

  // first, verify the reporting host is valid
  if (m->get_orig_source().is_osd()) {
    int from = m->get_orig_source().num();
    if (!osdmap.exists(from) ||
	osdmap.get_addr(from) != m->get_orig_source_inst().addr ||
	osdmap.is_down(from)) {
      dout(5) << "preprocess_failure from dead osd" << from << ", ignoring" << dendl;
      send_incremental(m->get_orig_source_inst(), m->get_epoch()+1);
      goto didit;
    }
  }
  

  // weird?
  if (!osdmap.have_inst(badboy)) {
    dout(5) << "preprocess_failure dne(/dup?): " << m->get_failed() << ", from " << m->get_orig_source_inst() << dendl;
    if (m->get_epoch() < osdmap.get_epoch())
      send_incremental(m->get_orig_source_inst(), m->get_epoch()+1);
    goto didit;
  }
  if (osdmap.get_inst(badboy) != m->get_failed()) {
    dout(5) << "preprocess_failure wrong osd: report " << m->get_failed() << " != map's " << osdmap.get_inst(badboy)
	    << ", from " << m->get_orig_source_inst() << dendl;
    if (m->get_epoch() < osdmap.get_epoch())
      send_incremental(m->get_orig_source_inst(), m->get_epoch()+1);
    goto didit;
  }
  // already reported?
  if (osdmap.is_down(badboy)) {
    dout(5) << "preprocess_failure dup: " << m->get_failed() << ", from " << m->get_orig_source_inst() << dendl;
    if (m->get_epoch() < osdmap.get_epoch())
      send_incremental(m->get_orig_source_inst(), m->get_epoch()+1);
    goto didit;
  }

  dout(10) << "preprocess_failure new: " << m->get_failed() << ", from " << m->get_orig_source_inst() << dendl;
  return false;

 didit:
  delete m;
  return true;
}

bool OSDMonitor::prepare_failure(MOSDFailure *m)
{
  stringstream ss;
  dout(1) << "prepare_failure " << m->get_failed() << " from " << m->get_orig_source_inst() << dendl;

  ss << m->get_failed() << " failed (by " << m->get_orig_source_inst() << ")";
  mon->get_logclient()->log(LOG_INFO, ss);
  
  // FIXME
  // take their word for it
  int badboy = m->get_failed().name.num();
  assert(osdmap.is_up(badboy));
  assert(osdmap.get_addr(badboy) == m->get_failed().addr);
  
  pending_inc.new_down[badboy] = false;
  
  paxos->wait_for_commit(new C_Reported(this, m));
  
  return true;
}

void OSDMonitor::_reported_failure(MOSDFailure *m)
{
  dout(7) << "_reported_failure on " << m->get_failed() << ", telling " << m->get_orig_source_inst() << dendl;
  send_latest(m->get_orig_source_inst(), m->get_epoch());
  delete m;
}


// boot --

bool OSDMonitor::preprocess_boot(MOSDBoot *m)
{
  if (ceph_fsid_compare(&m->sb.fsid, &mon->monmap->fsid)) {
    dout(0) << "preprocess_boot on fsid " << m->sb.fsid << " != " << mon->monmap->fsid << dendl;
    delete m;
    return true;
  }

  assert(m->get_orig_source_inst().name.is_osd());
  int from = m->get_orig_source_inst().name.num();
  
  // already booted?
  if (osdmap.is_up(from) &&
      osdmap.get_inst(from) == m->get_orig_source_inst()) {
    // yup.
    dout(7) << "preprocess_boot dup from " << m->get_orig_source_inst()
	    << " == " << osdmap.get_inst(from) << dendl;
    _booted(m, false);
    return true;
  }

  dout(10) << "preprocess_boot from " << m->get_orig_source_inst() << dendl;
  return false;
}

bool OSDMonitor::prepare_boot(MOSDBoot *m)
{
  dout(7) << "prepare_boot from " << m->get_orig_source_inst() << " sb " << m->sb << dendl;

  assert(m->get_orig_source().is_osd());
  int from = m->get_orig_source().num();
  
  // does this osd exist?
  if (!osdmap.exists(from)) {
    dout(1) << "boot from non-existent osd" << from << ", increase max_osd?" << dendl;
    delete m;
    return false;
  }

  // already up?  mark down first?
  if (osdmap.is_up(from)) {
    dout(7) << "prepare_boot was up, first marking down " << osdmap.get_inst(from) << dendl;
    assert(osdmap.get_inst(from) != m->get_orig_source_inst());  // preproces should have caught it
    
    // mark previous guy down
    pending_inc.new_down[from] = false;
    
    paxos->wait_for_commit(new C_RetryMessage(this, m));
  } else {
    // mark new guy up.
    down_pending_out.erase(from);  // if any
    pending_inc.new_up[from] = m->get_orig_source_addr();
    
    // mark in?
    pending_inc.new_weight[from] = CEPH_OSD_IN;

    if (m->sb.weight)
      osd_weight[from] = m->sb.weight;

    // adjust last clean unmount epoch?
    const osd_info_t& info = osdmap.get_info(from);
    dout(10) << " old osd_info: " << info << dendl;
    if (m->sb.mounted > info.last_clean_first ||
	(m->sb.mounted == info.last_clean_first &&
	 m->sb.clean_thru > info.last_clean_last)) {
      epoch_t first = m->sb.mounted;
      epoch_t last = m->sb.clean_thru;

      // adjust clean interval forward to the epoch the osd was actually marked down.
      if (info.up_from == first &&
	  (info.down_at-1) > last)
	last = info.down_at-1;

      dout(10) << "prepare_boot osd" << from << " last_clean_interval "
	       << info.last_clean_first << "-" << info.last_clean_last
	       << " -> " << first << "-" << last
	       << dendl;
      pending_inc.new_last_clean_interval[from] = pair<epoch_t,epoch_t>(first, last);
    }

    // wait
    paxos->wait_for_commit(new C_Booted(this, m));
  }
  return true;
}

void OSDMonitor::_booted(MOSDBoot *m, bool logit)
{
  dout(7) << "_booted " << m->get_orig_source_inst() 
	  << " w " << m->sb.weight << " from " << m->sb.current_epoch << dendl;
  send_latest(m->get_orig_source_inst(), m->sb.current_epoch+1);

  stringstream ss;
  ss << m->get_orig_source_inst() << " boot";
  mon->get_logclient()->log(LOG_INFO, ss);

  delete m;
}


// -------------
// in

bool OSDMonitor::preprocess_alive(MOSDAlive *m)
{
  int from = m->get_orig_source().num();
  if (osdmap.is_up(from) &&
      osdmap.get_inst(from) == m->get_orig_source_inst() &&
      osdmap.get_up_thru(from) >= m->map_epoch) {
    // yup.
    dout(7) << "preprocess_alive e" << m->map_epoch << " dup from " << m->get_orig_source_inst() << dendl;
    _alive(m);
    return true;
  }
  
  dout(10) << "preprocess_alive e" << m->map_epoch
	   << " from " << m->get_orig_source_inst() << dendl;
  return false;
}

bool OSDMonitor::prepare_alive(MOSDAlive *m)
{
  int from = m->get_orig_source().num();

  if (0) {  // we probably don't care much about these
    stringstream ss;
    ss << m->get_orig_source_inst() << " alive";
    mon->get_logclient()->log(LOG_DEBUG, ss);
  }

  dout(7) << "prepare_alive e" << m->map_epoch << " from " << m->get_orig_source_inst() << dendl;
  pending_inc.new_up_thru[from] = m->map_epoch;
  paxos->wait_for_commit(new C_Alive(this,m ));
  return true;
}

void OSDMonitor::_alive(MOSDAlive *m)
{
  dout(7) << "_alive e" << m->map_epoch
	  << " from " << m->get_orig_source_inst()
	  << dendl;
  send_latest(m->get_orig_source_inst(), m->map_epoch);
  delete m;
}


// ---

bool OSDMonitor::preprocess_remove_snaps(MRemoveSnaps *m)
{
  dout(7) << "preprocess_remove_snaps " << *m << dendl;
  
  for (map<int, vector<snapid_t> >::iterator q = m->snaps.begin();
       q != m->snaps.end();
       q++) {
    if (!osdmap.have_pg_pool(q->first)) {
      dout(10) << " ignoring removed_snaps " << q->second << " on non-existent pool " << q->first << dendl;
      continue;
    }
    const pg_pool_t& pi = osdmap.get_pg_pool(q->first);
    for (vector<snapid_t>::iterator p = q->second.begin(); 
	 p != q->second.end();
	 p++) {
      if (*p > pi.get_snap_seq() ||
	  !pi.removed_snaps.contains(*p))
	return false;
    }
  }
  delete m;
  return true;
}

bool OSDMonitor::prepare_remove_snaps(MRemoveSnaps *m)
{
  dout(7) << "prepare_remove_snaps " << *m << dendl;

  for (map<int, vector<snapid_t> >::iterator p = m->snaps.begin(); 
       p != m->snaps.end();
       p++) {
    pg_pool_t& pi = osdmap.pools[p->first];
    for (vector<snapid_t>::iterator q = p->second.begin();
	 q != p->second.end();
	 q++) {
      if (!pi.removed_snaps.contains(*q) &&
	  (!pending_inc.new_pools.count(p->first) ||
	   !pending_inc.new_pools[p->first].removed_snaps.contains(*q))) {
	if (pending_inc.new_pools.count(p->first) == 0)
	  pending_inc.new_pools[p->first] = pi;
	pg_pool_t& newpi = pending_inc.new_pools[p->first];
	newpi.removed_snaps.insert(*q);
	dout(10) << " pool " << p->first << " removed_snaps added " << *q
		 << " (now " << newpi.removed_snaps << ")" << dendl;
	if (*q > newpi.get_snap_seq()) {
	  dout(10) << " pool " << p->first << " snap_seq " << newpi.get_snap_seq() << " -> " << *q << dendl;
	  newpi.set_snap_seq(*q);
	}
	newpi.set_snap_epoch(pending_inc.epoch);
      }
    }
  }

  delete m;
  return true;
}


// ---------------
// map helpers

void OSDMonitor::send_to_waiting()
{
  dout(10) << "send_to_waiting " << osdmap.get_epoch() << dendl;

  map<entity_inst_t,epoch_t>::iterator i = waiting_for_map.begin();
  while (i != waiting_for_map.end()) {
    if (i->second) {
      if (i->second <= osdmap.get_epoch())
	send_incremental(i->first, i->second);
      else {
	dout(10) << "send_to_waiting skipping " << i->first
		 << " wants " << i->second
		 << dendl;
	i++;
	continue;
      }
    } else
      send_full(i->first);

    waiting_for_map.erase(i++);
  }
}

void OSDMonitor::send_latest(entity_inst_t who, epoch_t start)
{
  if (paxos->is_readable()) {
    dout(5) << "send_latest to " << who << " start " << start << " now" << dendl;
    if (start == 0)
      send_full(who);
    else
      send_incremental(who, start);
  } else {
    dout(5) << "send_latest to " << who << " start " << start << " later" << dendl;
    waiting_for_map[who] = start;
  }
}


void OSDMonitor::send_full(entity_inst_t who)
{
  dout(5) << "send_full to " << who << dendl;
  mon->messenger->send_message(new MOSDMap(mon->monmap->fsid, &osdmap), who);
}

void OSDMonitor::send_incremental(entity_inst_t dest, epoch_t from)
{
  dout(5) << "send_incremental from " << from << " -> " << osdmap.get_epoch()
	  << " to " << dest << dendl;
  
  MOSDMap *m = new MOSDMap(mon->monmap->fsid);
  
  for (epoch_t e = osdmap.get_epoch();
       e >= from;
       e--) {
    bufferlist bl;
    if (mon->store->get_bl_sn(bl, "osdmap", e) > 0) {
      dout(20) << "send_incremental    inc " << e << " " << bl.length() << " bytes" << dendl;
      m->incremental_maps[e] = bl;
    } 
    else if (mon->store->get_bl_sn(bl, "osdmap_full", e) > 0) {
      dout(20) << "send_incremental   full " << e << dendl;
      m->maps[e] = bl;
    }
    else {
      assert(0);  // we should have all maps.
    }
  }
  
  mon->messenger->send_message(m, dest);
}


void OSDMonitor::bcast_latest_mds()
{
  epoch_t e = osdmap.get_epoch();
  dout(1) << "bcast_latest_mds epoch " << e << dendl;
  
  // tell mds
  set<int> up;
  mon->mdsmon()->mdsmap.get_up_mds_set(up);
  for (set<int>::iterator i = up.begin();
       i != up.end();
       i++) {
    send_incremental(mon->mdsmon()->mdsmap.get_inst(*i), osdmap.get_epoch());
  }
}

void OSDMonitor::bcast_latest_osd()
{
  epoch_t e = osdmap.get_epoch();
  dout(1) << "bcast_latest_osd epoch " << e << dendl;

  // tell osds
  set<int32_t> osds;
  osdmap.get_all_osds(osds);
  for (set<int32_t>::iterator it = osds.begin();
       it != osds.end();
       it++) {
    if (osdmap.is_down(*it)) continue;
    
    send_incremental(osdmap.get_inst(*it), osdmap.get_epoch());
  }  
}

void OSDMonitor::bcast_full_osd()
{
  epoch_t e = osdmap.get_epoch();
  dout(1) << "bcast_full_osd epoch " << e << dendl;

  // tell osds
  set<int32_t> osds;
  osdmap.get_all_osds(osds);
  for (set<int32_t>::iterator it = osds.begin();
       it != osds.end();
       it++) {
    if (osdmap.is_down(*it)) continue;
    send_full(osdmap.get_inst(*it));
  }  
}



void OSDMonitor::blacklist(entity_addr_t a, utime_t until)
{
  dout(10) << "blacklist " << a << " until " << until << dendl;
  pending_inc.new_blacklist[a] = until;
}



// TICK


void OSDMonitor::tick()
{
  if (!paxos->is_active()) return;

  update_from_paxos();
  dout(10) << osdmap << dendl;

  if (!mon->is_leader()) return;

  bool do_propose = false;

  // mark down osds out?
  utime_t now = g_clock.now();
  map<int,utime_t>::iterator i = down_pending_out.begin();
  while (i != down_pending_out.end()) {
    int o = i->first;
    utime_t down = now;
    down -= i->second;
    i++;

    if (osdmap.is_down(o) && osdmap.is_in(o)) {
      if (down.sec() >= g_conf.mon_osd_down_out_interval) {
	dout(10) << "tick marking osd" << o << " OUT after " << down
		 << " sec (target " << g_conf.mon_osd_down_out_interval << ")" << dendl;
	pending_inc.new_weight[o] = CEPH_OSD_OUT;
	do_propose = true;
	
	stringstream ss;
	ss << "osd" << o << " out (down for " << down << ")";
	mon->get_logclient()->log(LOG_INFO, ss);
      } else
	continue;
    }

    down_pending_out.erase(o);
  }

  // expire blacklisted items?
  for (hash_map<entity_addr_t,utime_t>::iterator p = osdmap.blacklist.begin();
       p != osdmap.blacklist.end();
       p++) {
    if (p->second < now) {
      dout(10) << "expiring blacklist item " << p->first << " expired " << p->second << " < now " << now << dendl;
      pending_inc.old_blacklist.push_back(p->first);
      do_propose = true;
    }
  }


  // ---------------
#define SWAP_PRIMARIES_AT_START 0
#define SWAP_TIME 1
#if 0
  if (SWAP_PRIMARIES_AT_START) {
    // For all PGs that have OSD 0 as the primary,
    // switch them to use the first replca
    ps_t numps = osdmap.get_pg_num();
    for (int pool=0; pool<1; pool++)
      for (ps_t ps = 0; ps < numps; ++ps) {
	pg_t pgid = pg_t(pg_t::TYPE_REP, ps, pool, -1);
	vector<int> osds;
	osdmap.pg_to_osds(pgid, osds); 
	if (osds[0] == 0) {
	  pending_inc.new_pg_swap_primary[pgid] = osds[1];
	  dout(3) << "Changing primary for PG " << pgid << " from " << osds[0] << " to "
		  << osds[1] << dendl;
	  do_propose = true;
	}
      }
  }
#endif
  // ---------------

  if (do_propose)
    propose_pending();
}



void OSDMonitor::mark_all_down()
{
  assert(mon->is_leader());

  dout(7) << "mark_all_down" << dendl;

  set<int32_t> ls;
  osdmap.get_all_osds(ls);
  for (set<int32_t>::iterator it = ls.begin();
       it != ls.end();
       it++) {
    if (osdmap.is_down(*it)) continue;
    pending_inc.new_down[*it] = true;  // FIXME: am i sure it's clean? we need a proper osd shutdown sequence!
  }

  propose_pending();
}



bool OSDMonitor::preprocess_command(MMonCommand *m)
{
  int r = -1;
  bufferlist rdata;
  stringstream ss;

  if (m->cmd.size() > 1) {
    if (m->cmd[1] == "stat") {
      osdmap.print_summary(ss);
      r = 0;
    }
    else if (m->cmd[1] == "dump") {
      OSDMap *p = &osdmap;
      if (m->cmd.size() > 2) {
	epoch_t e = atoi(m->cmd[2].c_str());
	bufferlist b;
	mon->store->get_bl_sn(b,"osdmap_full", e);
	if (!b.length()) {
	  p = 0;
	  r = -ENOENT;
	} else {
	  p = new OSDMap;
	  p->decode(b);
	}
      }
      if (p) {
	stringstream ds;
	p->print(ds);
	rdata.append(ds);
	ss << "dumped osdmap epoch " << p->get_epoch();
	if (p != &osdmap)
	  delete p;
	r = 0;
      }
    }
    else if (m->cmd[1] == "getmap") {
      osdmap.encode(rdata);
      ss << "got osdmap epoch " << osdmap.get_epoch();
      r = 0;
    }
    else if (m->cmd[1] == "getcrushmap") {
      osdmap.crush.encode(rdata);
      ss << "got crush map from osdmap epoch " << osdmap.get_epoch();
      r = 0;
    }
    else if (m->cmd[1] == "getmaxosd") {
      ss << "max_osd = " << osdmap.get_max_osd() << " in epoch " << osdmap.get_epoch();
      r = 0;
    }
    else if (m->cmd[1] == "injectargs" && m->cmd.size() == 4) {
      if (m->cmd[2] == "*") {
	for (int i=0; i<osdmap.get_max_osd(); i++)
	  if (osdmap.is_up(i))
	    mon->inject_args(osdmap.get_inst(i), m->cmd[3]);
	r = 0;
	ss << "ok bcast";
      } else {
	errno = 0;
	int who = strtol(m->cmd[2].c_str(), 0, 10);
	if (!errno && who >= 0 && osdmap.is_up(who)) {
	  mon->inject_args(osdmap.get_inst(who), m->cmd[3]);
	  r = 0;
	  ss << "ok";
	} else 
	  ss << "specify osd number or *";
      }
    }
    else if (m->cmd[1] == "scrub" && m->cmd.size() > 2) {
      if (m->cmd[2] == "*") {
	ss << "osds ";
	int c = 0;
	for (int i=0; i<osdmap.get_max_osd(); i++)
	  if (osdmap.is_up(i)) {
	    ss << (c++ ? ",":"") << i;
	    mon->messenger->send_message(new MOSDScrub(osdmap.get_fsid()),
					 osdmap.get_inst(i));
	  }	    
	r = 0;
	ss << " instructed to scrub";
      } else {
	long osd = strtol(m->cmd[2].c_str(), 0, 10);
	if (osdmap.is_up(osd)) {
	  mon->messenger->send_message(new MOSDScrub(osdmap.get_fsid()),
				       osdmap.get_inst(osd));
	  r = 0;
	  ss << "osd" << osd << " instructed to scrub";
	} else 
	  ss << "osd" << osd << " is not up";
      }
    }

  }
  if (r != -1) {
    string rs;
    getline(ss, rs);
    mon->reply_command(m, r, rs, rdata);
    return true;
  } else
    return false;
}

bool OSDMonitor::prepare_command(MMonCommand *m)
{
  stringstream ss;
  string rs;
  int err = -EINVAL;
  if (m->cmd.size() > 1) {
    if (m->cmd[1] == "setcrushmap") {
      dout(10) << "prepare_command setting new crush map" << dendl;
      pending_inc.crush = m->get_data();
      string rs = "set crush map";
      paxos->wait_for_commit(new Monitor::C_Command(mon, m, 0, rs));
      return true;
    }
    else if (m->cmd[1] == "setmap" && m->cmd.size() == 3) {
      OSDMap map;
      map.decode(m->get_data());
      epoch_t e = atoi(m->cmd[2].c_str());
      if (ceph_fsid_compare(&map.fsid, &mon->monmap->fsid) == 0) {
	if (pending_inc.epoch == e) {
	  map.set_epoch(pending_inc.epoch);  // make sure epoch is correct
	  map.encode(pending_inc.fullmap);
	  string rs = "set osd map";
	  paxos->wait_for_commit(new Monitor::C_Command(mon, m, 0, rs));
	  return true;
	} else
	  ss << "next osdmap epoch " << pending_inc.epoch << " != " << e;
      } else
	  ss << "osdmap fsid " << map.fsid << " does not match monitor fsid " << mon->monmap->fsid;
      err = -EINVAL;
    }
    else if (m->cmd[1] == "setmaxosd" && m->cmd.size() > 2) {
      pending_inc.new_max_osd = atoi(m->cmd[2].c_str());
      ss << "set new max_osd = " << pending_inc.new_max_osd;
      getline(ss, rs);
      paxos->wait_for_commit(new Monitor::C_Command(mon, m, 0, rs));
      return true;
    }
    else if (m->cmd[1] == "down" && m->cmd.size() == 3) {
      long osd = strtol(m->cmd[2].c_str(), 0, 10);
      if (!osdmap.exists(osd)) {
	ss << "osd" << osd << " does not exist";
      } else if (osdmap.is_down(osd)) {
	ss << "osd" << osd << " is already down";
      } else {
	pending_inc.new_down[osd] = false;
	ss << "marked down osd" << osd;
	getline(ss, rs);
	paxos->wait_for_commit(new Monitor::C_Command(mon, m, 0, rs));
	
	// send them the new map when it updates, so they know it
	waiting_for_map[osdmap.get_inst(osd)] = osdmap.get_epoch();

	return true;
      }
    }
    else if (m->cmd[1] == "out" && m->cmd.size() == 3) {
      long osd = strtol(m->cmd[2].c_str(), 0, 10);
      if (!osdmap.exists(osd)) {
	ss << "osd" << osd << " does not exist";
      } else if (osdmap.is_out(osd)) {
	ss << "osd" << osd << " is already out";
      } else {
	pending_inc.new_weight[osd] = CEPH_OSD_OUT;
	ss << "marked out osd" << osd;
	getline(ss, rs);
	paxos->wait_for_commit(new Monitor::C_Command(mon, m, 0, rs));
	return true;
      } 
    }
    else if (m->cmd[1] == "in" && m->cmd.size() == 3) {
      long osd = strtol(m->cmd[2].c_str(), 0, 10);
      if (osdmap.is_in(osd)) {
	ss << "osd" << osd << " is already in";
      } else if (!osdmap.exists(osd)) {
	ss << "osd" << osd << " does not exist";
      } else {
	pending_inc.new_weight[osd] = CEPH_OSD_IN;
	ss << "marked in osd" << osd;
	getline(ss, rs);
	paxos->wait_for_commit(new Monitor::C_Command(mon, m, 0, rs));
	return true;
      } 
    }
    else if (m->cmd[1] == "reweight" && m->cmd.size() == 4) {
      long osd = strtol(m->cmd[2].c_str(), 0, 10);
      float w = strtof(m->cmd[3].c_str(), 0);
      long ww = (int)((float)CEPH_OSD_IN*w);
      if (osdmap.exists(osd)) {
	pending_inc.new_weight[osd] = ww;
	ss << "reweighted osd" << osd << " to " << w << " (" << ios::hex << ww << ios::dec << ")";
	getline(ss, rs);
	paxos->wait_for_commit(new Monitor::C_Command(mon, m, 0, rs));
	return true;
      } 
    }
    else if (m->cmd[1] == "lost" && m->cmd.size() >= 3) {
      long osd = strtol(m->cmd[2].c_str(), 0, 10);
      if (m->cmd.size() < 4 ||
	  m->cmd[3] != "--yes-i-really-mean-it") {
	ss << "are you SURE?  this might mean real, permanent data loss.  pass --yes-i-really-mean-it if you really do.";
      }
      else if (!osdmap.exists(osd) || !osdmap.is_down(osd)) {
	ss << "osd" << osd << " is not down or doesn't exist";
      } else {
	epoch_t e = osdmap.get_info(osd).down_at;
	pending_inc.new_lost[osd] = e;
	ss << "marked osd lost in epoch " << e;
	getline(ss, rs);
	paxos->wait_for_commit(new Monitor::C_Command(mon, m, 0, rs));
	return true;
      }
    }
    else if (m->cmd[1] == "pool" && m->cmd.size() >= 3) {
      if (m->cmd.size() >= 5 && m->cmd[2] == "mksnap") {
	int pool = osdmap.lookup_pg_pool_name(m->cmd[3].c_str());
	if (pool < 0) {
	  ss << "unrecognized pool '" << m->cmd[3] << "'";
	  err = -ENOENT;
	} else {
	  const pg_pool_t *p = &osdmap.get_pg_pool(pool);
	  pg_pool_t *pp = 0;
	  if (pending_inc.new_pools.count(pool))
	    pp = &pending_inc.new_pools[pool];
	  const string& snapname = m->cmd[4];
	  if (p->snap_exists(snapname.c_str()) ||
	      (pp && pp->snap_exists(snapname.c_str()))) {
	    ss << "pool " << m->cmd[3] << " snap " << snapname << " already exists";
	    err = -EEXIST;
	  } else {
	    if (!pp) {
	      pp = &pending_inc.new_pools[pool];
	      *pp = *p;
	    }
	    pp->add_snap(snapname.c_str(), g_clock.now());
	    pp->set_snap_epoch(pending_inc.epoch);
	    ss << "created pool " << m->cmd[3] << " snap " << snapname;
	    getline(ss, rs);
	    paxos->wait_for_commit(new Monitor::C_Command(mon, m, 0, rs));
	    return true;
	  }
	}
      }
      else if (m->cmd.size() >= 5 && m->cmd[2] == "rmsnap") {
	int pool = osdmap.lookup_pg_pool_name(m->cmd[3].c_str());
	if (pool < 0) {
	  ss << "unrecognized pool '" << m->cmd[3] << "'";
	  err = -ENOENT;
	} else {
	  const pg_pool_t *p = &osdmap.get_pg_pool(pool);
	  pg_pool_t *pp = 0;
	  if (pending_inc.new_pools.count(pool))
	    pp = &pending_inc.new_pools[pool];
	  const string& snapname = m->cmd[4];
	  if (!p->snap_exists(snapname.c_str()) &&
	      (!pp || !pp->snap_exists(snapname.c_str()))) {
	    ss << "pool " << m->cmd[3] << " snap " << snapname << " does not exists";
	    err = -ENOENT;
	  } else {
	    if (!pp) {
	      pp = &pending_inc.new_pools[pool];
	      *pp = *p;
	    }
	    snapid_t sn = pp->snap_exists(snapname.c_str());
	    pp->remove_snap(sn);
	    pp->set_snap_epoch(pending_inc.epoch);
	    ss << "removed pool " << m->cmd[3] << " snap " << snapname;
	    getline(ss, rs);
	    paxos->wait_for_commit(new Monitor::C_Command(mon, m, 0, rs));
	    return true;
	  }
	}

      }
      else if (m->cmd[2] == "create" && m->cmd.size() >= 4) {
	int pool = 1;
	for (map<int,nstring>::iterator i = osdmap.pool_name.begin();
	     i != osdmap.pool_name.end();
	     i++) {
	  if (i->second == m->cmd[3]) {
	    ss << "pool '" << i->second << "' exists";
	    err = -EEXIST;
	    goto out;
	  }
	  if (i->first >= pool)
	    pool = i->first + 1;
	}
	pending_inc.new_pools[pool].v.type = CEPH_PG_TYPE_REP;
	pending_inc.new_pools[pool].v.size = 2;
	pending_inc.new_pools[pool].v.crush_ruleset = 0;
	pending_inc.new_pools[pool].v.pg_num = 8;
	pending_inc.new_pools[pool].v.pgp_num = 8;
	pending_inc.new_pools[pool].v.lpg_num = 0;
	pending_inc.new_pools[pool].v.lpgp_num = 0;
	pending_inc.new_pools[pool].v.last_change = pending_inc.epoch;
	pending_inc.new_pool_names[pool] = m->cmd[3];
	ss << "pool '" << m->cmd[3] << "' created";
	getline(ss, rs);
	paxos->wait_for_commit(new Monitor::C_Command(mon, m, 0, rs));
	return true;
      } else if (m->cmd[2] == "set") {
	int pool = osdmap.lookup_pg_pool_name(m->cmd[3].c_str());
	if (pool < 0) {
	  ss << "unrecognized pool '" << m->cmd[3] << "'";
	  err = -ENOENT;
	} else {
	  const pg_pool_t *p = &osdmap.get_pg_pool(pool);
	  int n = atoi(m->cmd[5].c_str());
	  if (n) {
	    if (m->cmd[4] == "size") {
	      pending_inc.new_pools[pool] = *p;
	      pending_inc.new_pools[pool].v.size = n;
	      ss << "set pool " << pool << " size to " << n;
	      getline(ss, rs);
	      paxos->wait_for_commit(new Monitor::C_Command(mon, m, 0, rs));
	      return true;
	    } else if (m->cmd[4] == "pg_num") {
	      if (n <= p->get_pg_num()) {
		ss << "specified pg_num " << n << " <= current " << p->get_pg_num();
	      } else if (!mon->pgmon()->pg_map.creating_pgs.empty()) {
		ss << "currently creating pgs, wait";
		err = -EAGAIN;
	      } else {
		pending_inc.new_pools[pool] = osdmap.pools[pool];
		pending_inc.new_pools[pool].v.pg_num = n;
		ss << "set pool " << pool << " pg_num to " << n;
		getline(ss, rs);
		paxos->wait_for_commit(new Monitor::C_Command(mon, m, 0, rs));
		return true;
	      }
	    } else if (m->cmd[4] == "pgp_num") {
	      if (n <= p->get_pgp_num()) {
		ss << "specified pgp_num " << n << " <= current " << p->get_pgp_num();
	      } else if (n > p->get_pg_num()) {
		ss << "specified pgp_num " << n << " > pg_num " << p->get_pg_num();
	      } else if (!mon->pgmon()->pg_map.creating_pgs.empty()) {
		ss << "still creating pgs, wait";
		err = -EAGAIN;
	      } else {
		pending_inc.new_pools[pool] = osdmap.pools[pool];
		pending_inc.new_pools[pool].v.pgp_num = n;
		ss << "set pool " << pool << " pgp_num to " << n;
		getline(ss, rs);
		paxos->wait_for_commit(new Monitor::C_Command(mon, m, 0, rs));
		return true;
	      }
	    } else {
	      ss << "unrecognized pool field " << m->cmd[4];
	    }
	  }
	}
      }
    }
    else {
      ss << "unknown command " << m->cmd[1];
    }
  } else {
    ss << "no command?";
  }
out:
  getline(ss, rs);
  mon->reply_command(m, err, rs);
  return false;
}

bool OSDMonitor::preprocess_pool_snap ( MPoolSnap *m) {
  if (m->pool < 0 ) {
    _pool_snap(m, -ENOENT, pending_inc.epoch);
    return true; //done with this message
  }
  bool snap_exists = false;
  pg_pool_t *pp = 0;
  if (pending_inc.new_pools.count(m->pool)) pp = &pending_inc.new_pools[m->pool];
  //check if the snapname exists
  if ((osdmap.get_pg_pool(m->pool).snap_exists(m->name.c_str())) ||
      (pp && pp->snap_exists(m->name.c_str()))) snap_exists = true;

  if (m->create) { //if it's a snap creation request
    if(snap_exists) {
      _pool_snap(m, -EEXIST, pending_inc.epoch);
      return true;
    }
    else return false; //this message needs to go through preparation
  }
  //it's a snap deletion request if we make it here
  if (!snap_exists) {
    _pool_snap(m, -ENOENT, pending_inc.epoch);
    return true; //done with this message
  }
  return false;
}

bool OSDMonitor::prepare_pool_snap ( MPoolSnap *m)
{
  const pg_pool_t *p = &osdmap.get_pg_pool(m->pool);
  pg_pool_t* pp = 0;
  //if the pool isn't already in the update, add it
  if (!pending_inc.new_pools.count(m->pool)) pending_inc.new_pools[m->pool] = *p;
  pp = &pending_inc.new_pools[m->pool];

  if (m->create) { //it's a snap creation message
    pp->add_snap(m->name.c_str(), g_clock.now());
    pp->set_snap_epoch(pending_inc.epoch);
  }
  else { //it's a snap removal message
    pp->remove_snap(pp->snap_exists(m->name.c_str()));
  }
  paxos->wait_for_commit(new OSDMonitor::C_Snap(this, m, 0, pending_inc.epoch));
  return true;
}

void OSDMonitor::_pool_snap(MPoolSnap *m, int replyCode, epoch_t epoch)
{
  MPoolSnapReply *reply = new MPoolSnapReply(m->fsid, m->tid, replyCode, epoch);
  mon->messenger->send_message(reply, m->get_orig_source_inst());
  delete m;
}
