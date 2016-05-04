/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#include "GdbCommand.h"

#include "ReplayTask.h"

using namespace std;

namespace rr {

static SimpleGdbCommand when("when", [](GdbServer&, Task* t,
                                        const vector<string>&) {
  if (t->session().is_replaying()) {
    return string("Current event: ") +
           to_string(static_cast<ReplayTask*>(t)->current_trace_frame().time());
  }
  return string("Current event not known (diversion?)");
});

static SimpleGdbCommand when_ticks("when-ticks", [](GdbServer&, Task* t,
                                                    const vector<string>&) {
  return string("Current tick: ") + to_string(t->tick_count());
});

static SimpleGdbCommand when_tid("when-tid", [](GdbServer&, Task* t,
                                                const vector<string>&) {
  return string("Current tid: ") + to_string(t->tid);
});

static int gNextCheckpointId = 0;

string invoke_checkpoint(GdbServer& gdb_server, Task*,
                         const vector<string>& args) {
  const string& where = args[1];
  int checkpoint_id = ++gNextCheckpointId;
  GdbServer::Checkpoint::Explicit e;
  if (gdb_server.timeline.can_add_checkpoint()) {
    e = GdbServer::Checkpoint::EXPLICIT;
  } else {
    e = GdbServer::Checkpoint::NOT_EXPLICIT;
  }
  gdb_server.checkpoints[checkpoint_id] = GdbServer::Checkpoint(
      gdb_server.timeline, gdb_server.last_continue_tuid, e, where);
  return string("Checkpoint ") + to_string(checkpoint_id) + " at " + where;
}
static SimpleGdbCommand checkpoint("checkpoint", invoke_checkpoint);

string invoke_delete_checkpoint(GdbServer& gdb_server, Task*,
                                const vector<string>& args) {
  int id = stoi(args[1]);
  auto it = gdb_server.checkpoints.find(id);
  if (it != gdb_server.checkpoints.end()) {
    if (it->second.is_explicit == GdbServer::Checkpoint::EXPLICIT) {
      gdb_server.timeline.remove_explicit_checkpoint(it->second.mark);
    }
    gdb_server.checkpoints.erase(it);
    return string("Deleted checkpoint ") + to_string(id) + ".";
  } else {
    return string("No checkpoint number ") + to_string(id) + ".";
  }
}
static SimpleGdbCommand delete_checkpoint("delete checkpoint",
                                          invoke_delete_checkpoint);

string invoke_info_checkpoints(GdbServer& gdb_server, Task*,
                               const vector<string>&) {
  if (gdb_server.checkpoints.size() == 0) {
    return "No checkpoints.";
  }
  string out = "ID\tWhen\tWhere";
  for (auto& c : gdb_server.checkpoints) {
    out += string("\n") + to_string(c.first) + "\t" +
           to_string(c.second.mark.time()) + "\t" + c.second.where;
  }
  return out;
}
static SimpleGdbCommand info_checkpoints("info checkpoints",
                                         invoke_info_checkpoints);

string invoke_set_condition(GdbServer& gdb_server, Task *t,
                            const vector<string>& args)
{
  (void)gdb_server;
  unsigned long val = stol(args[1]);
  char *ptr = t->read_mem(t->condition_pages);
  t->write_mem(remote_ptr<unsigned long>((uintptr_t)ptr), val);
  return string("wrote ") + to_string(val) + ".";
}

static SimpleGdbCommand set_condition("set-condition",
                                      invoke_set_condition);

/*static*/ void GdbCommand::init_auto_args() {
  checkpoint.add_auto_arg("rr-where");
}

} // namespace rr
