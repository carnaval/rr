/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#include "record_syscall.h"

#include <asm/prctl.h>
#include <arpa/inet.h>
#include <asm/ldt.h>
#include <assert.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/ethtool.h>
#include <linux/futex.h>
#include <linux/if.h>
#include <linux/ipc.h>
#include <linux/msg.h>
#include <linux/net.h>
#include <linux/perf_event.h>
#include <linux/personality.h>
#include <linux/prctl.h>
#include <linux/seccomp.h>
#include <linux/sem.h>
#include <linux/shm.h>
#include <linux/sockios.h>
#include <linux/videodev2.h>
#include <linux/wireless.h>
#include <linux/msdos_fs.h>
#include <poll.h>
#include <sched.h>
#include <sound/asound.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/quota.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <termios.h>

#include <limits>
#include <sstream>
#include <utility>

#include <rr/rr.h>

#include "preload/preload_interface.h"

#include "AutoRemoteSyscalls.h"
#include "drm.h"
#include "Flags.h"
#include "ftrace.h"
#include "kernel_abi.h"
#include "kernel_metadata.h"
#include "kernel_supplement.h"
#include "log.h"
#include "ProcMemMonitor.h"
#include "RecordSession.h"
#include "RecordTask.h"
#include "Scheduler.h"
#include "StdioMonitor.h"
#include "TraceStream.h"
#include "util.h"
#include "VirtualPerfCounterMonitor.h"

using namespace std;

namespace rr {

union _semun {
  int val;
  struct semid64_ds* buf;
  unsigned short int* array;
  struct seminfo* __buf;
};

/* We can't include <sys/shm.h> to get shmctl because it clashes with
 * linux/shm.h.
 */
static int _shmctl(int shmid, int cmd, shmid64_ds* buf) {
  if (sizeof(void*) == 4) {
    cmd |= IPC_64;
  }
#ifdef SYS_shmctl
  return syscall(SYS_shmctl, shmid, cmd, buf);
#else
  return syscall(SYS_ipc, SHMCTL, shmid, cmd, 0, buf);
#endif
}
static int _semctl(int semid, int semnum, int cmd, _semun un_arg) {
  if (sizeof(void*) == 4) {
    cmd |= IPC_64;
  }
#ifdef SYS_semctl
  return syscall(SYS_semctl, semid, semnum, cmd, un_arg);
#else
  return syscall(SYS_ipc, SEMCTL, semid, semnum, cmd, &un_arg);
#endif
}

/**
 * Modes used to register syscall memory parameter with TaskSyscallState.
 */
enum ArgMode {
  // Syscall memory parameter is an in-parameter only.
  // This is only important when we want to move the buffer to scratch memory
  // so we can modify it without making the modifications potentially visible
  // to user code. Otherwise, such parameters can be ignored.
  IN,
  // Syscall memory parameter is out-parameter only.
  OUT,
  // Syscall memory parameter is an in-out parameter.
  IN_OUT,
  // Syscall memory parameter is an in-out parameter but we must not use
  // scratch (e.g. for futexes, we must use the actual memory word).
  IN_OUT_NO_SCRATCH
};

/**
 * Specifies how to determine the size of a syscall memory
 * parameter. There is usually an incoming size determined before the syscall
 * executes (which we need in order to allocate scratch memory), combined
 * with an optional final size taken from the syscall result or a specific
 * memory location after the syscall has executed. The minimum of the incoming
 * and final sizes is used, if both are present.
 */
struct ParamSize {
  ParamSize(size_t incoming_size = size_t(-1))
      : incoming_size(incoming_size), from_syscall(false) {}
  /**
   * p points to a tracee location that is already initialized with a
   * "maximum buffer size" passed in by the tracee, and which will be filled
   * in with the size of the data by the kernel when the syscall exits.
   */
  template <typename T>
  static ParamSize from_initialized_mem(RecordTask* t, remote_ptr<T> p) {
    ParamSize r(p.is_null() ? size_t(0) : size_t(t->read_mem(p)));
    r.mem_ptr = p;
    r.read_size = sizeof(T);
    return r;
  }
  /**
   * p points to a tracee location which will be filled in with the size of
   * the data by the kernel when the syscall exits, but the location
   * is uninitialized before the syscall.
   */
  template <typename T> static ParamSize from_mem(remote_ptr<T> p) {
    ParamSize r(size_t(-1));
    r.mem_ptr = p;
    r.read_size = sizeof(T);
    return r;
  }
  /**
   * When the syscall exits, the syscall result will be of type T and contain
   * the size of the data. 'incoming_size', if present, is a bound on the size
   * of the data.
   */
  template <typename T>
  static ParamSize from_syscall_result(size_t incoming_size = size_t(-1)) {
    ParamSize r(incoming_size);
    r.from_syscall = true;
    r.read_size = sizeof(T);
    return r;
  }
  /**
   * Indicate that the size will be at most 'max'.
   */
  ParamSize limit_size(size_t max) const {
    ParamSize r(*this);
    r.incoming_size = min(r.incoming_size, max);
    return r;
  }

  /**
   * Return true if 'other' takes its dynamic size from the same source as
   * this.
   * When multiple syscall memory parameters take their dynamic size from the
   * same source, the source size is distributed among them, with the first
   * registered parameter taking up to its max_size bytes, followed by the next,
   * etc. This lets us efficiently record iovec buffers.
   */
  bool is_same_source(const ParamSize& other) const {
    return ((!mem_ptr.is_null() && other.mem_ptr == mem_ptr) ||
            (from_syscall && other.from_syscall)) &&
           (read_size == other.read_size);
  }
  /**
   * Compute the actual size after the syscall has executed.
   * 'already_consumed' bytes are subtracted from the syscall-result/
   * memory-location part of the size.
   */
  size_t eval(RecordTask* t, size_t already_consumed) const;

  size_t incoming_size;
  /** If non-null, the size is limited by the value at this location after
   *  the syscall. */
  remote_ptr<void> mem_ptr;
  /** Size of the value at mem_ptr or in the syscall result register. */
  size_t read_size;
  /** If true, the size is limited by the value of the syscall result. */
  bool from_syscall;
};

size_t ParamSize::eval(RecordTask* t, size_t already_consumed) const {
  size_t s = incoming_size;
  if (!mem_ptr.is_null()) {
    size_t mem_size;
    switch (read_size) {
      case 4:
        mem_size = t->read_mem(mem_ptr.cast<uint32_t>());
        break;
      case 8:
        mem_size = t->read_mem(mem_ptr.cast<uint64_t>());
        break;
      default:
        ASSERT(t, false) << "Unknown read_size";
        return 0;
    }
    ASSERT(t, already_consumed <= mem_size);
    s = min(s, mem_size - already_consumed);
  }
  if (from_syscall) {
    size_t syscall_size = max<ssize_t>(0, t->regs().syscall_result_signed());
    switch (read_size) {
      case 4:
        syscall_size = uint32_t(syscall_size);
        break;
      case 8:
        syscall_size = uint64_t(syscall_size);
        break;
      default:
        ASSERT(t, false) << "Unknown read_size";
        return 0;
    }
    ASSERT(t, already_consumed <= syscall_size);
    s = min(s, syscall_size - already_consumed);
  }
  ASSERT(t, s < size_t(-1));
  return s;
}

/**
 * When tasks enter syscalls that may block and so must be
 * prepared for a context-switch, and the syscall params
 * include (in)outparams that point to buffers, we need to
 * redirect those arguments to scratch memory.  This allows rr
 * to serialize execution of what may be multiple blocked
 * syscalls completing "simultaneously" (from rr's
 * perspective).  After the syscall exits, we restore the data
 * saved in scratch memory to the original buffers.
 *
 * Then during replay, we simply restore the saved data to the
 * tracee's passed-in buffer args and continue on.
 *
 * This is implemented by having rec_prepare_syscall_arch set up
 * a record in param_list for syscall in-memory  parameter (whether
 * "in" or "out"). Then done_preparing is called, which does the actual
 * scratch setup. process_syscall_results is called when the syscall is
 * done, to write back scratch results to the real parameters and
 * clean everything up.
 *
 * ... a fly in this ointment is may-block buffered syscalls.
 * If a task blocks in one of those, it will look like it just
 * entered a syscall that needs a scratch buffer.  However,
 * it's too late at that point to fudge the syscall args,
 * because processing of the syscall has already begun in the
 * kernel.  But that's OK: the syscallbuf code has already
 * swapped out the original buffer-pointers for pointers into
 * the syscallbuf (which acts as its own scratch memory).  We
 * just have to worry about setting things up properly for
 * replay.
 *
 * The descheduled syscall will "abort" its commit into the
 * syscallbuf, so the outparam data won't actually be saved
 * there (and thus, won't be restored during replay).  During
 * replay, we have to restore them like we restore the
 * non-buffered-syscall scratch data. This is done by recording
 * the relevant syscallbuf record data in rec_process_syscall_arch.
 */
struct TaskSyscallState {
  void init(RecordTask* t) {
    if (preparation_done) {
      return;
    }
    this->t = t;
    scratch = t->scratch_ptr;
  }

  /**
   * Identify a syscall memory parameter whose address is in register 'arg'
   * with type T.
   * Returns a remote_ptr to the data in the child (before scratch relocation)
   * or null if parameters have already been prepared (the syscall is
   * resuming).
   */
  template <typename T>
  remote_ptr<T> reg_parameter(int arg, ArgMode mode = OUT) {
    return reg_parameter(arg, sizeof(T), mode).cast<T>();
  }
  /**
   * Identify a syscall memory parameter whose address is in register 'arg'
   * with size 'size'.
   * Returns a remote_ptr to the data in the child (before scratch relocation)
   * or null if parameters have already been prepared (the syscall is
   * resuming).
   */
  remote_ptr<void> reg_parameter(int arg, const ParamSize& size,
                                 ArgMode mode = OUT);
  /**
   * Identify a syscall memory parameter whose address is in memory at
   * location 'addr_of_buf_ptr' with type T.
   * Returns a remote_ptr to the data in the child (before scratch relocation)
   * or null if parameters have already been prepared (the syscall is
   * resuming).
   * addr_of_buf_ptr must be in a buffer identified by some init_..._parameter
   * call.
   */
  template <typename T>
  remote_ptr<T> mem_ptr_parameter(remote_ptr<void> addr_of_buf_ptr,
                                  ArgMode mode = OUT) {
    return mem_ptr_parameter(addr_of_buf_ptr, sizeof(T), mode).cast<T>();
  }
  /**
   * Identify a syscall memory parameter whose address is in memory at
   * location 'addr_of_buf_ptr' with type T.
   * Returns a remote_ptr to the data in the child (before scratch relocation)
   * or null if parameters have already been prepared (the syscall is
   * resuming).
   * addr_of_buf_ptr must be in a buffer identified by some init_..._parameter
   * call.
   */
  template <typename Ptr>
  remote_ptr<typename Ptr::Referent> mem_ptr_parameter_inferred(
      remote_ptr<Ptr> addr_of_buf_ptr, ArgMode mode = OUT) {
    remote_ptr<void> p =
        mem_ptr_parameter(addr_of_buf_ptr, Ptr::referent_size(), mode);
    return p.cast<typename Ptr::Referent>();
  }
  /**
   * Identify a syscall memory parameter whose address is in memory at
   * location 'addr_of_buf_ptr' with size 'size'.
   * Returns a remote_ptr to the data in the child (before scratch relocation)
   * or null if parameters have already been prepared (the syscall is
   * resuming).
   * addr_of_buf_ptr must be in a buffer identified by some init_..._parameter
   * call.
   */
  remote_ptr<void> mem_ptr_parameter(remote_ptr<void> addr_of_buf_ptr,
                                     const ParamSize& size, ArgMode mode = OUT);

  typedef void (*AfterSyscallAction)(RecordTask* t);
  void after_syscall_action(AfterSyscallAction action) {
    after_syscall_actions.push_back(action);
  }

  void emulate_result(uint64_t result) {
    ASSERT(t, !preparation_done);
    ASSERT(t, !should_emulate_result);
    should_emulate_result = true;
    emulated_result = result;
  }

  /**
   * Internal method that takes 'ptr', an address within some memory parameter,
   * and relocates it to the parameter's location in scratch memory.
   */
  remote_ptr<void> relocate_pointer_to_scratch(remote_ptr<void> ptr);
  /**
   * Internal method that takes the index of a MemoryParam and a vector
   * containing the actual sizes assigned to each param < param_index, and
   * computes the actual size to use for parameter param_index.
   */
  size_t eval_param_size(size_t param_index, vector<size_t>& actual_sizes);
  /**
   * Called when all memory parameters have been identified. If 'sw' is
   * ALLOW_SWITCH, sets up scratch memory and updates registers etc as
   * necessary.
   * If scratch can't be used for some reason, returns PREVENT_SWITCH,
   * otherwise returns 'sw'.
   */
  Switchable done_preparing(Switchable sw);
  enum WriteBack { WRITE_BACK, NO_WRITE_BACK };
  /**
   * Called when a syscall exits to copy results from scratch memory to their
   * original destinations, update registers, etc.
   */
  void process_syscall_results();

  /**
   * Upon successful syscall completion, each RestoreAndRecordScratch record
   * in param_list consumes num_bytes from the t->scratch_ptr
   * buffer, copying the data to remote_dest and recording the data at
   * remote_dest. If ptr_in_reg is greater than zero, updates the task's
   * ptr_in_reg register with 'remote_dest'. If ptr_in_memory is non-null,
   * updates the ptr_in_memory location with the value 'remote_dest'.
   */
  struct MemoryParam {
    MemoryParam() : ptr_in_reg(0) {}

    remote_ptr<void> dest;
    remote_ptr<void> scratch;
    ParamSize num_bytes;
    remote_ptr<void> ptr_in_memory;
    int ptr_in_reg;
    ArgMode mode;
  };

  RecordTask* t;

  vector<MemoryParam> param_list;
  /** Tracks the position in t's scratch_ptr buffer where we should allocate
   *  the next scratch area.
   */
  remote_ptr<void> scratch;

  vector<AfterSyscallAction> after_syscall_actions;

  std::unique_ptr<TraceTaskEvent> exec_saved_event;

  RecordTask* ptraced_tracee;

  /** Saved syscall-entry registers, used by code paths that modify the
   *  registers temporarily.
   */
  Registers syscall_entry_registers;

  /** When nonzero, syscall is expected to return the given errno and we should
   *  die if it does not. This is set when we detect an error condition during
   *  syscall-enter preparation.
   */
  int expect_errno;

  /** When should_emulate_result is true, syscall result should be adjusted to
   *  be emulated_result. */
  bool should_emulate_result;
  uint64_t emulated_result;

  /** Records whether the syscall is switchable. Only valid when
   *  preparation_done is true. */
  Switchable switchable;

  /** Whether we should write back the syscall results from scratch. Only
   *  valid when preparation_done is true. */
  WriteBack write_back;

  /** When true, this syscall has already been prepared and should not
   *  be set up again.
   */
  bool preparation_done;
  /** When true, the scratch area is enabled, otherwise we're letting
   *  syscall outputs be written directly to their destinations.
   *  Only valid when preparation_done is true.
   */
  bool scratch_enabled;

  /** Miscellaneous saved data that can be used by particular syscalls */
  vector<uint8_t> saved_data;

  TaskSyscallState()
      : t(nullptr),
        ptraced_tracee(nullptr),
        expect_errno(0),
        should_emulate_result(false),
        preparation_done(false),
        scratch_enabled(false) {}
};

static const Property<TaskSyscallState, RecordTask> syscall_state_property;

template <typename Arch>
static void set_remote_ptr_arch(RecordTask* t, remote_ptr<void> addr,
                                remote_ptr<void> value) {
  auto typed_addr = addr.cast<typename Arch::unsigned_word>();
  t->write_mem(typed_addr, (typename Arch::unsigned_word)value.as_int());
}

static void set_remote_ptr(RecordTask* t, remote_ptr<void> addr,
                           remote_ptr<void> value) {
  RR_ARCH_FUNCTION(set_remote_ptr_arch, t->arch(), t, addr, value);
}

template <typename Arch>
static remote_ptr<void> get_remote_ptr_arch(RecordTask* t,
                                            remote_ptr<void> addr) {
  auto typed_addr = addr.cast<typename Arch::unsigned_word>();
  auto old = t->read_mem(typed_addr);
  return remote_ptr<void>(old);
}

static remote_ptr<void> get_remote_ptr(RecordTask* t, remote_ptr<void> addr) {
  RR_ARCH_FUNCTION(get_remote_ptr_arch, t->arch(), t, addr);
}

static void align_scratch(remote_ptr<void>* scratch, uintptr_t amount = 8) {
  *scratch = (scratch->as_int() + amount - 1) & ~(amount - 1);
}

remote_ptr<void> TaskSyscallState::reg_parameter(int arg, const ParamSize& size,
                                                 ArgMode mode) {
  if (preparation_done) {
    return remote_ptr<void>();
  }

  MemoryParam param;
  param.dest = t->regs().arg(arg);
  if (param.dest.is_null()) {
    return remote_ptr<void>();
  }
  param.num_bytes = size;
  param.mode = mode;
  if (mode != IN_OUT_NO_SCRATCH) {
    param.scratch = scratch;
    scratch += param.num_bytes.incoming_size;
    align_scratch(&scratch);
    param.ptr_in_reg = arg;
  }
  param_list.push_back(param);
  return param.dest;
}

remote_ptr<void> TaskSyscallState::mem_ptr_parameter(
    remote_ptr<void> addr_of_buf_ptr, const ParamSize& size, ArgMode mode) {
  if (preparation_done) {
    return remote_ptr<void>();
  }

  MemoryParam param;
  param.dest = get_remote_ptr(t, addr_of_buf_ptr);
  if (param.dest.is_null()) {
    return remote_ptr<void>();
  }
  param.num_bytes = size;
  param.mode = mode;
  if (mode != IN_OUT_NO_SCRATCH) {
    param.scratch = scratch;
    scratch += param.num_bytes.incoming_size;
    align_scratch(&scratch);
    param.ptr_in_memory = addr_of_buf_ptr;
  }
  param_list.push_back(param);
  return param.dest;
}

remote_ptr<void> TaskSyscallState::relocate_pointer_to_scratch(
    remote_ptr<void> ptr) {
  int num_relocations = 0;
  remote_ptr<void> result;
  for (auto& param : param_list) {
    if (param.dest <= ptr && ptr < param.dest + param.num_bytes.incoming_size) {
      result = param.scratch + (ptr - param.dest);
      ++num_relocations;
    }
  }
  assert(num_relocations > 0 &&
         "Pointer in non-scratch memory being updated to point to scratch?");
  assert(num_relocations <= 1 &&
         "Overlapping buffers containing relocated pointer?");
  return result;
}

Switchable TaskSyscallState::done_preparing(Switchable sw) {
  if (preparation_done) {
    return switchable;
  }
  preparation_done = true;
  write_back = WRITE_BACK;

  ssize_t scratch_num_bytes = scratch - t->scratch_ptr;
  ASSERT(t, scratch_num_bytes >= 0);
  if (sw == ALLOW_SWITCH && scratch_num_bytes > t->scratch_size) {
    LOG(warn)
        << "`" << t->syscall_name(t->ev().Syscall().number)
        << "' needed a scratch buffer of size " << scratch_num_bytes
        << ", but only " << t->scratch_size
        << " was available.  Disabling context switching: deadlock may follow.";
    switchable = PREVENT_SWITCH;
  } else {
    switchable = sw;
  }
  if (switchable == PREVENT_SWITCH || param_list.empty()) {
    return switchable;
  }

  scratch_enabled = true;

  // Step 1: Copy all IN/IN_OUT parameters to their scratch areas
  for (auto& param : param_list) {
    ASSERT(t, param.num_bytes.incoming_size < size_t(-1));
    if (param.mode == IN_OUT || param.mode == IN) {
      // Initialize scratch buffer with input data
      uint8_t buf[param.num_bytes.incoming_size];
      t->read_bytes_helper(param.dest, param.num_bytes.incoming_size, buf);
      t->write_bytes_helper(param.scratch, param.num_bytes.incoming_size, buf);
    }
  }
  // Step 2: Update pointers in registers/memory to point to scratch areas
  Registers r = t->regs();
  for (auto& param : param_list) {
    if (param.ptr_in_reg) {
      r.set_arg(param.ptr_in_reg, param.scratch.as_int());
    }
    if (!param.ptr_in_memory.is_null()) {
      // Pointers being relocated must themselves be in scratch memory.
      // We don't want to modify non-scratch memory. Find the pointer's location
      // in scratch memory.
      auto p = relocate_pointer_to_scratch(param.ptr_in_memory);
      // Update pointer to point to scratch.
      // Note that this can only happen after step 1 is complete and all
      // parameter data has been copied to scratch memory.
      set_remote_ptr(t, p, param.scratch);
    }
    // If the number of bytes to record is coming from a memory location,
    // update that location to scratch.
    if (!param.num_bytes.mem_ptr.is_null()) {
      param.num_bytes.mem_ptr =
          relocate_pointer_to_scratch(param.num_bytes.mem_ptr);
    }
  }
  t->set_regs(r);
  return switchable;
}

size_t TaskSyscallState::eval_param_size(size_t i,
                                         vector<size_t>& actual_sizes) {
  assert(actual_sizes.size() == i);

  size_t already_consumed = 0;
  for (size_t j = 0; j < i; ++j) {
    if (param_list[j].num_bytes.is_same_source(param_list[i].num_bytes)) {
      already_consumed += actual_sizes[j];
    }
  }
  size_t size = param_list[i].num_bytes.eval(t, already_consumed);
  actual_sizes.push_back(size);
  return size;
}

void TaskSyscallState::process_syscall_results() {
  ASSERT(t, preparation_done);

  // XXX what's the best way to handle failed syscalls? Currently we just
  // record everything as if it succeeded. That handles failed syscalls that
  // wrote partial results, but doesn't handle syscalls that failed with
  // EFAULT.
  vector<size_t> actual_sizes;
  if (scratch_enabled) {
    size_t scratch_num_bytes = scratch - t->scratch_ptr;
    auto data = t->read_mem(t->scratch_ptr.cast<uint8_t>(), scratch_num_bytes);
    Registers r = t->regs();
    // Step 1: compute actual sizes of all buffers and copy outputs
    // from scratch back to their origin
    for (size_t i = 0; i < param_list.size(); ++i) {
      auto& param = param_list[i];
      size_t size = eval_param_size(i, actual_sizes);
      if (write_back == WRITE_BACK &&
          (param.mode == IN_OUT || param.mode == OUT)) {
        const uint8_t* d = data.data() + (param.scratch - t->scratch_ptr);
        t->write_bytes_helper(param.dest, size, d);
      }
    }
    bool memory_cleaned_up = false;
    // Step 2: restore modified in-memory pointers and registers
    for (size_t i = 0; i < param_list.size(); ++i) {
      auto& param = param_list[i];
      if (param.ptr_in_reg) {
        r.set_arg(param.ptr_in_reg, param.dest.as_int());
      }
      if (!param.ptr_in_memory.is_null()) {
        memory_cleaned_up = true;
        set_remote_ptr(t, param.ptr_in_memory, param.dest);
      }
    }
    if (write_back == WRITE_BACK) {
      // Step 3: record all output memory areas
      for (size_t i = 0; i < param_list.size(); ++i) {
        auto& param = param_list[i];
        size_t size = actual_sizes[i];
        if (param.mode == IN_OUT_NO_SCRATCH) {
          t->record_remote(param.dest, size);
        } else if (param.mode == IN_OUT || param.mode == OUT) {
          // If pointers in memory were fixed up in step 2, then record
          // from tracee memory to ensure we record such fixes. Otherwise we
          // can record from our local data.
          // XXX This optimization can be improved if necessary...
          if (memory_cleaned_up) {
            t->record_remote(param.dest, size);
          } else {
            const uint8_t* d = data.data() + (param.scratch - t->scratch_ptr);
            t->record_local(param.dest, size, d);
          }
        }
      }
    }
    t->set_regs(r);
  } else {
    for (size_t i = 0; i < param_list.size(); ++i) {
      auto& param = param_list[i];
      size_t size = eval_param_size(i, actual_sizes);
      t->record_remote(param.dest, size);
    }
  }

  if (should_emulate_result) {
    Registers r = t->regs();
    r.set_syscall_result(emulated_result);
    t->set_regs(r);
  }

  for (auto& action : after_syscall_actions) {
    action(t);
  }
}

template <typename Arch>
static void prepare_recvmsg(RecordTask* t, TaskSyscallState& syscall_state,
                            remote_ptr<typename Arch::msghdr> msgp,
                            const ParamSize& io_size) {
  auto namelen_ptr = REMOTE_PTR_FIELD(msgp, msg_namelen);
  syscall_state.mem_ptr_parameter(
      REMOTE_PTR_FIELD(msgp, msg_name),
      ParamSize::from_initialized_mem(t, namelen_ptr));

  auto msg = t->read_mem(msgp);
  remote_ptr<void> iovecsp_void = syscall_state.mem_ptr_parameter(
      REMOTE_PTR_FIELD(msgp, msg_iov),
      sizeof(typename Arch::iovec) * msg.msg_iovlen, IN);
  auto iovecsp = iovecsp_void.cast<typename Arch::iovec>();
  auto iovecs = t->read_mem(iovecsp, msg.msg_iovlen);
  for (size_t i = 0; i < msg.msg_iovlen; ++i) {
    syscall_state.mem_ptr_parameter(REMOTE_PTR_FIELD(iovecsp + i, iov_base),
                                    io_size.limit_size(iovecs[i].iov_len));
  }

  auto controllen_ptr = REMOTE_PTR_FIELD(msgp, msg_controllen);
  syscall_state.mem_ptr_parameter(
      REMOTE_PTR_FIELD(msgp, msg_control),
      ParamSize::from_initialized_mem(t, controllen_ptr));
}

template <typename Arch>
static void prepare_recvmmsg(RecordTask* t, TaskSyscallState& syscall_state,
                             remote_ptr<typename Arch::mmsghdr> mmsgp,
                             unsigned int vlen) {
  for (unsigned int i = 0; i < vlen; ++i) {
    auto msgp = mmsgp + i;
    prepare_recvmsg<Arch>(t, syscall_state, REMOTE_PTR_FIELD(msgp, msg_hdr),
                          ParamSize::from_mem(REMOTE_PTR_FIELD(msgp, msg_len)));
  }
}

template <typename Arch>
static Switchable prepare_socketcall(RecordTask* t,
                                     TaskSyscallState& syscall_state) {
  /* int socketcall(int call, unsigned long *args) {
   *   long a[6];
   *   copy_from_user(a,args);
   *   sys_recv(a0, (void __user *)a1, a[2], a[3]);
   * }
   *
   *  (from http://lxr.linux.no/#linux+v3.6.3/net/socket.c#L2354)
   */
  switch ((int)t->regs().arg1_signed()) {
    /* int socket(int domain, int type, int protocol); */
    case SYS_SOCKET:
    /* int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
     */
    case SYS_CONNECT:
    /* int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen); */
    case SYS_BIND:
    /* int listen(int sockfd, int backlog) */
    case SYS_LISTEN:
    /* ssize_t send(int sockfd, const void *buf, size_t len, int flags) */
    case SYS_SEND:
    /* ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const
     * struct sockaddr *dest_addr, socklen_t addrlen); */
    case SYS_SENDTO:
    /* int setsockopt(int sockfd, int level, int optname, const void *optval,
     * socklen_t optlen); */
    case SYS_SETSOCKOPT:
    /* int shutdown(int socket, int how) */
    case SYS_SHUTDOWN:
      break;

    /*  int getsockopt(int sockfd, int level, int optname, const void *optval,
     * socklen_t* optlen);
     */
    case SYS_GETSOCKOPT: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::getsockopt_args>(2, IN);
      auto optlen_ptr = syscall_state.mem_ptr_parameter_inferred(
          REMOTE_PTR_FIELD(argsp, optlen), IN_OUT);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, optval),
          ParamSize::from_initialized_mem(t, optlen_ptr));
      break;
    }

    /* int socketpair(int domain, int type, int protocol, int sv[2]);
     *
     * values returned in sv
     */
    case SYS_SOCKETPAIR: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::socketpair_args>(2, IN);
      syscall_state.mem_ptr_parameter(REMOTE_PTR_FIELD(argsp, sv),
                                      sizeof(int) * 2);
      break;
    }

    /* int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
     */
    case SYS_GETPEERNAME:
    /* int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
     */
    case SYS_GETSOCKNAME: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::getsockname_args>(2, IN);
      auto addrlen_ptr = syscall_state.mem_ptr_parameter_inferred(
          REMOTE_PTR_FIELD(argsp, addrlen), IN_OUT);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, addr),
          ParamSize::from_initialized_mem(t, addrlen_ptr));
      break;
    }

    /* ssize_t recv([int sockfd, void *buf, size_t len, int flags]) */
    case SYS_RECV: {
      auto argsp = syscall_state.reg_parameter<typename Arch::recv_args>(2, IN);
      auto args = t->read_mem(argsp);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, buf),
          ParamSize::from_syscall_result<typename Arch::ssize_t>(args.len));
      return ALLOW_SWITCH;
    }

    /* int accept([int sockfd, struct sockaddr *addr, socklen_t *addrlen]) */
    case SYS_ACCEPT: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::accept_args>(2, IN);
      auto addrlen_ptr = syscall_state.mem_ptr_parameter_inferred(
          REMOTE_PTR_FIELD(argsp, addrlen), IN_OUT);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, addr),
          ParamSize::from_initialized_mem(t, addrlen_ptr));
      return ALLOW_SWITCH;
    }

    /* int accept4([int sockfd, struct sockaddr *addr, socklen_t *addrlen, int
     * flags]) */
    case SYS_ACCEPT4: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::accept4_args>(2, IN);
      auto addrlen_ptr = syscall_state.mem_ptr_parameter_inferred(
          REMOTE_PTR_FIELD(argsp, addrlen), IN_OUT);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, addr),
          ParamSize::from_initialized_mem(t, addrlen_ptr));
      return ALLOW_SWITCH;
    }

    case SYS_RECVFROM: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::recvfrom_args>(2, IN);
      auto args = t->read_mem(argsp);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, buf),
          ParamSize::from_syscall_result<typename Arch::ssize_t>(args.len));
      auto addrlen_ptr = syscall_state.mem_ptr_parameter_inferred(
          REMOTE_PTR_FIELD(argsp, addrlen), IN_OUT);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, src_addr),
          ParamSize::from_initialized_mem(t, addrlen_ptr));
      return ALLOW_SWITCH;
    }

    case SYS_RECVMSG: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::recvmsg_args>(2, IN);
      auto msgp = syscall_state.mem_ptr_parameter_inferred(
          REMOTE_PTR_FIELD(argsp, msg), IN_OUT);
      prepare_recvmsg<Arch>(
          t, syscall_state, msgp,
          ParamSize::from_syscall_result<typename Arch::ssize_t>());

      auto args = t->read_mem(argsp);
      if (!(args.flags & MSG_DONTWAIT)) {
        return ALLOW_SWITCH;
      }
      break;
    }

    case SYS_RECVMMSG: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::recvmmsg_args>(2, IN);
      auto args = t->read_mem(argsp);
      remote_ptr<void> mmsgp_void = syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, msgvec),
          sizeof(typename Arch::mmsghdr) * args.vlen, IN_OUT);
      auto mmsgp = mmsgp_void.cast<typename Arch::mmsghdr>();
      prepare_recvmmsg<Arch>(t, syscall_state, mmsgp, args.vlen);
      if (!(args.flags & MSG_DONTWAIT)) {
        return ALLOW_SWITCH;
      }
      break;
    }

    /* ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) */
    case SYS_SENDMSG: {
      auto argsp = remote_ptr<typename Arch::sendmsg_args>(t->regs().arg2());
      auto args = t->read_mem(argsp);
      if (!(args.flags & MSG_DONTWAIT)) {
        return ALLOW_SWITCH;
      }
      break;
    }

    case SYS_SENDMMSG: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::sendmmsg_args>(2, IN);
      auto args = t->read_mem(argsp);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, msgvec),
          sizeof(typename Arch::mmsghdr) * args.vlen, IN_OUT);
      if (!(args.flags & MSG_DONTWAIT)) {
        return ALLOW_SWITCH;
      }
      break;
    }

    default:
      syscall_state.expect_errno = EINVAL;
      break;
  }
  return PREVENT_SWITCH;
}

template <typename Arch>
static Switchable prepare_msgctl(TaskSyscallState& syscall_state, int cmd,
                                 int ptr_reg) {
  switch (cmd) {
    case IPC_STAT:
    case MSG_STAT:
      syscall_state.reg_parameter<typename Arch::msqid64_ds>(ptr_reg);
      break;
    case IPC_INFO:
    case MSG_INFO:
      syscall_state.reg_parameter<typename Arch::msginfo>(ptr_reg);
      break;

    case IPC_SET:
    case IPC_RMID:
      break;

    default:
      syscall_state.expect_errno = EINVAL;
      break;
  }
  return PREVENT_SWITCH;
}

template <typename Arch>
static Switchable prepare_shmctl(TaskSyscallState& syscall_state, int cmd,
                                 int ptr_reg) {
  switch (cmd) {
    case IPC_SET:
    case IPC_RMID:
    case SHM_LOCK:
    case SHM_UNLOCK:
      break;

    case IPC_STAT:
    case SHM_STAT:
      syscall_state.reg_parameter<typename Arch::shmid64_ds>(ptr_reg);
      break;

    case IPC_INFO:
      syscall_state.reg_parameter<typename Arch::shminfo64>(ptr_reg);
      break;

    case SHM_INFO:
      syscall_state.reg_parameter<typename Arch::shm_info>(ptr_reg);
      break;

    default:
      syscall_state.expect_errno = EINVAL;
      break;
  }
  return PREVENT_SWITCH;
}

enum SemctlDereference { DEREFERENCE, USE_DIRECTLY };

template <typename Arch>
static Switchable prepare_semctl(RecordTask* t, TaskSyscallState& syscall_state,
                                 int semid, int cmd, int ptr_reg,
                                 SemctlDereference deref) {
  switch (cmd) {
    case IPC_SET:
    case IPC_RMID:
    case GETNCNT:
    case GETPID:
    case GETVAL:
    case GETZCNT:
    case SETALL:
    case SETVAL:
      break;

    case IPC_STAT:
    case SEM_STAT:
      if (deref == DEREFERENCE) {
        syscall_state.mem_ptr_parameter<typename Arch::semid64_ds>(
            syscall_state.reg_parameter<typename Arch::unsigned_long>(ptr_reg));
      } else {
        syscall_state.reg_parameter<typename Arch::semid64_ds>(ptr_reg);
      }
      break;

    case IPC_INFO:
    case SEM_INFO:
      if (deref == DEREFERENCE) {
        syscall_state.mem_ptr_parameter<typename Arch::seminfo>(
            syscall_state.reg_parameter<typename Arch::unsigned_long>(ptr_reg));
      } else {
        syscall_state.reg_parameter<typename Arch::seminfo>(ptr_reg);
      }
      break;

    case GETALL: {
      semid64_ds ds;
      _semun un_arg;
      un_arg.buf = &ds;
      int ret = _semctl(semid, 0, IPC_STAT, un_arg);
      ASSERT(t, ret == 0);

      ParamSize size = sizeof(unsigned short) * ds.sem_nsems;
      if (deref == DEREFERENCE) {
        syscall_state.mem_ptr_parameter(
            syscall_state.reg_parameter<typename Arch::unsigned_long>(ptr_reg),
            size);
      } else {
        syscall_state.reg_parameter(ptr_reg, size);
      }
      break;
    }

    default:
      syscall_state.expect_errno = EINVAL;
      break;
  }
  return PREVENT_SWITCH;
}

/**
 * A change has been made to file 'fd' in task t. If the file has been mmapped
 * somewhere in t's address space, record the changes.
 * We check for matching files by comparing file names. This may not be
 * reliable but hopefully it's good enough for the cases where we need this.
 * This doesn't currently handle shared mappings very well. A file mapped
 * shared in multiple locations will be recorded once per location.
 * This doesn't handle mappings of the file into other address spaces.
 */
static void record_file_change(RecordTask* t, int fd, uint64_t offset,
                               uint64_t length) {
  string file_name = t->file_name_of_fd(fd);

  for (auto m : t->vm()->maps()) {
    if (m.map.fsname() == file_name) {
      uint64_t start = max(offset, uint64_t(m.map.file_offset_bytes()));
      uint64_t end = min(offset + length,
                         uint64_t(m.map.file_offset_bytes()) + m.map.size());
      if (start < end) {
        t->record_remote(m.map.start() + (start - m.map.file_offset_bytes()),
                         end - start);
      }
    }
  };
}

template <typename Arch>
static void record_v4l2_buffer_contents(RecordTask* t) {
  remote_ptr<typename Arch::v4l2_buffer> bufp = t->regs().arg3();
  auto buf = t->read_mem(bufp);

  switch (buf.memory) {
    case V4L2_MEMORY_MMAP:
      record_file_change(t, (int)t->regs().arg1_signed(), buf.m.offset,
                         buf.length);
      return;

    default:
      ASSERT(t, false) << "Unhandled V4L2 memory type " << buf.memory;
      return;
  }
}

template <typename Arch> static void record_usbdevfs_reaped_urb(RecordTask* t) {
  if (t->regs().syscall_failed()) {
    return;
  }

  remote_ptr<typename Arch::unsigned_word> pp = t->regs().arg3();
  remote_ptr<typename Arch::usbdevfs_urb> p = t->read_mem(pp);
  t->record_remote(p);
  auto urb = t->read_mem(p);
  size_t length;
  if (urb.type == USBDEVFS_URB_TYPE_ISO) {
    auto iso_frame_descs_ptr = REMOTE_PTR_FIELD(p, iso_frame_desc[0]);
    auto iso_frame_descs =
        t->read_mem(iso_frame_descs_ptr, urb.number_of_packets);
    length = 0;
    for (auto& f : iso_frame_descs) {
      length += f.length;
    }
    t->record_local(iso_frame_descs_ptr, iso_frame_descs.data(),
                    iso_frame_descs.size());
  } else {
    length = urb.buffer_length;
  }
  // It's tempting to use actual_length here but in some cases the kernel
  // writes back more data than that.
  t->record_remote(urb.buffer, length);
}

static void record_page_below_stack_ptr(RecordTask* t) {
  /* Record.the page above the top of |t|'s stack.  The SIOC* ioctls
   * have been observed to write beyond the end of tracees' stacks, as
   * if they had allocated scratch space for themselves.  All we can do
   * for now is try to record the scratch data.
   */
  t->record_remote(t->regs().sp() - page_size(), page_size());
}

#define IOCTL_MASK_SIZE(v) ((v) & ~(_IOC_SIZEMASK << _IOC_SIZESHIFT))

template <typename Arch>
static Switchable prepare_ioctl(RecordTask* t,
                                TaskSyscallState& syscall_state) {
  int fd = t->regs().arg1();
  uint64_t result;
  if (t->fd_table()->emulate_ioctl(fd, t, &result)) {
    // Don't perform this syscall.
    Registers r = t->regs();
    r.set_arg1(-1);
    t->set_regs(r);
    syscall_state.emulate_result(result);
    return PREVENT_SWITCH;
  }

  unsigned long request = t->regs().arg2();
  int type = _IOC_TYPE(request);
  int nr = _IOC_NR(request);
  int dir = _IOC_DIR(request);
  int size = _IOC_SIZE(request);

  LOG(debug) << "handling ioctl(" << HEX(request) << "): type:" << HEX(type)
             << " nr:" << HEX(nr) << " dir:" << HEX(dir) << " size:" << size;

  ASSERT(t, !t->is_desched_event_syscall())
      << "Failed to skip past desched ioctl()";

  /* Some ioctl()s are irregular and don't follow the _IOC()
   * conventions.  Special case them here. */
  switch (request) {
    case SIOCETHTOOL: {
      auto ifrp = syscall_state.reg_parameter<typename Arch::ifreq>(3, IN);
      syscall_state.mem_ptr_parameter<typename Arch::ethtool_cmd>(
          REMOTE_PTR_FIELD(ifrp, ifr_ifru.ifru_data));
      syscall_state.after_syscall_action(record_page_below_stack_ptr);
      return PREVENT_SWITCH;
    }

    case SIOCGIFCONF: {
      auto ifconfp =
          syscall_state.reg_parameter<typename Arch::ifconf>(3, IN_OUT);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(ifconfp, ifc_ifcu.ifcu_buf),
          ParamSize::from_initialized_mem(t,
                                          REMOTE_PTR_FIELD(ifconfp, ifc_len)));
      syscall_state.after_syscall_action(record_page_below_stack_ptr);
      return PREVENT_SWITCH;
    }

    case SIOCGIFADDR:
    case SIOCGIFHWADDR:
    case SIOCGIFFLAGS:
    case SIOCGIFINDEX:
    case SIOCGIFMTU:
    case SIOCGIFNAME:
      syscall_state.reg_parameter<typename Arch::ifreq>(3);
      syscall_state.after_syscall_action(record_page_below_stack_ptr);
      return PREVENT_SWITCH;

    case SIOCGIWRATE:
      // SIOCGIWRATE hasn't been observed to write beyond
      // tracees' stacks, but we record a stack page here
      // just in case the behavior is driver-dependent.
      syscall_state.reg_parameter<typename Arch::iwreq>(3);
      syscall_state.after_syscall_action(record_page_below_stack_ptr);
      return PREVENT_SWITCH;

    case TCGETS:
      syscall_state.reg_parameter<typename Arch::termios>(3);
      return PREVENT_SWITCH;

    case TIOCINQ:
      syscall_state.reg_parameter<int>(3);
      return PREVENT_SWITCH;

    case TIOCGWINSZ:
      syscall_state.reg_parameter<typename Arch::winsize>(3);
      return PREVENT_SWITCH;

    case TIOCGPGRP:
      syscall_state.reg_parameter<typename Arch::pid_t>(3);
      return PREVENT_SWITCH;

    case SNDRV_CTL_IOCTL_PVERSION:
      syscall_state.reg_parameter<int>(3);
      return PREVENT_SWITCH;

    case SNDRV_CTL_IOCTL_CARD_INFO:
      syscall_state.reg_parameter<typename Arch::snd_ctl_card_info>(3);
      return PREVENT_SWITCH;
  }

  /* In ioctl language, "_IOC_READ" means "outparam".  Both
   * READ and WRITE can be set for inout params.
   * USBDEVFS ioctls seem to be mostly backwards in their interpretation of the
   * read/write bits :-(.
   */
  if (!(_IOC_READ & dir)) {
    switch (IOCTL_MASK_SIZE(request)) {
      case IOCTL_MASK_SIZE(BTRFS_IOC_CLONE):
      case IOCTL_MASK_SIZE(BTRFS_IOC_CLONE_RANGE):
      case IOCTL_MASK_SIZE(FIOCLEX):
      case IOCTL_MASK_SIZE(FIONCLEX):
      case IOCTL_MASK_SIZE(USBDEVFS_DISCARDURB):
      case IOCTL_MASK_SIZE(USBDEVFS_RESET):
        return PREVENT_SWITCH;
      case IOCTL_MASK_SIZE(USBDEVFS_GETDRIVER):
        // Reads and writes its parameter despite not having the _IOC_READ bit.
        syscall_state.reg_parameter(3, size);
        return PREVENT_SWITCH;
      case IOCTL_MASK_SIZE(USBDEVFS_REAPURB):
      case IOCTL_MASK_SIZE(USBDEVFS_REAPURBNDELAY):
        syscall_state.reg_parameter(3, size);
        syscall_state.after_syscall_action(record_usbdevfs_reaped_urb<Arch>);
        return ALLOW_SWITCH;
    }
    /* If the kernel isn't going to write any data back to
     * us, we hope and pray that the result of the ioctl
     * (observable to the tracee) is deterministic.
     * We're also assuming it doesn't block.
     * XXX this is far too risky! Many ioctls use irregular ioctl codes
     * that do not have the _IOC_READ bit set but actually do write to
     * user-space! */
    LOG(debug) << "  (presumed ignorable ioctl, nothing to do)";
    return PREVENT_SWITCH;
  }

  /* The following are thought to be "regular" ioctls, the
   * processing of which is only known to (observably) write to
   * the bytes in the structure passed to the kernel.  So all we
   * need is to record |size| bytes.
   * Since the size may vary across architectures we mask it out here to check
   * only the type + number. */
  switch (IOCTL_MASK_SIZE(request)) {
    case IOCTL_MASK_SIZE(VIDIOC_QUERYCAP):
    case IOCTL_MASK_SIZE(VIDIOC_ENUM_FMT):
    case IOCTL_MASK_SIZE(VIDIOC_G_FMT):
    case IOCTL_MASK_SIZE(VIDIOC_S_FMT):
    case IOCTL_MASK_SIZE(VIDIOC_TRY_FMT):
    case IOCTL_MASK_SIZE(VIDIOC_G_PARM):
    case IOCTL_MASK_SIZE(VIDIOC_S_PARM):
    case IOCTL_MASK_SIZE(VIDIOC_REQBUFS):
    case IOCTL_MASK_SIZE(VIDIOC_QUERYBUF):
    case IOCTL_MASK_SIZE(VIDIOC_QBUF):
    case IOCTL_MASK_SIZE(VIDIOC_G_CTRL):
    case IOCTL_MASK_SIZE(VIDIOC_S_CTRL):
    case IOCTL_MASK_SIZE(VFAT_IOCTL_READDIR_BOTH):
      syscall_state.reg_parameter(3, size, IN_OUT);
      return PREVENT_SWITCH;

    case IOCTL_MASK_SIZE(TIOCGPTN):
    case IOCTL_MASK_SIZE(USBDEVFS_GET_CAPABILITIES):
      syscall_state.reg_parameter(3, size);
      return PREVENT_SWITCH;

    case IOCTL_MASK_SIZE(USBDEVFS_ALLOC_STREAMS):
    case IOCTL_MASK_SIZE(USBDEVFS_CLAIMINTERFACE):
    case IOCTL_MASK_SIZE(USBDEVFS_CLEAR_HALT):
    case IOCTL_MASK_SIZE(USBDEVFS_DISCONNECT_CLAIM):
    case IOCTL_MASK_SIZE(USBDEVFS_FREE_STREAMS):
    case IOCTL_MASK_SIZE(USBDEVFS_RELEASEINTERFACE):
    case IOCTL_MASK_SIZE(USBDEVFS_SETCONFIGURATION):
    case IOCTL_MASK_SIZE(USBDEVFS_SETINTERFACE):
    case IOCTL_MASK_SIZE(USBDEVFS_SUBMITURB):
      // Doesn't actually seem to write to userspace
      return PREVENT_SWITCH;

    case IOCTL_MASK_SIZE(USBDEVFS_IOCTL): {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::usbdevfs_ioctl>(3, IN);
      auto args = t->read_mem(argsp);
      syscall_state.mem_ptr_parameter(REMOTE_PTR_FIELD(argsp, data),
                                      _IOC_SIZE(args.ioctl_code));
      return PREVENT_SWITCH;
    }
    case IOCTL_MASK_SIZE(USBDEVFS_CONTROL): {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::usbdevfs_ctrltransfer>(3,
                                                                            IN);
      auto args = t->read_mem(argsp);
      syscall_state.mem_ptr_parameter(REMOTE_PTR_FIELD(argsp, data),
                                      args.wLength);
      return PREVENT_SWITCH;
    }
  }

  /* These ioctls are mostly regular but require additional recording. */
  switch (IOCTL_MASK_SIZE(request)) {
    case IOCTL_MASK_SIZE(VIDIOC_DQBUF): {
      if (size == sizeof(typename Arch::v4l2_buffer)) {
        syscall_state.reg_parameter(3, size, IN_OUT);
        syscall_state.after_syscall_action(record_v4l2_buffer_contents<Arch>);
        // VIDIOC_DQBUF can block. It can't if the fd was opened O_NONBLOCK,
        // but we don't try to determine that.
        // Note that we're exposed to potential race conditions here because
        // VIDIOC_DQBUF (blocking or not) assumes the driver has filled
        // the mmapped data region at some point since the buffer was queued
        // with VIDIOC_QBUF, and we don't/can't know exactly when that
        // happened. Replay could fail if this thread or another thread reads
        // the contents of mmapped contents queued with the driver.
        return ALLOW_SWITCH;
      }
    }
  }

  syscall_state.expect_errno = EINVAL;
  return PREVENT_SWITCH;
}

static bool maybe_emulate_wait(RecordTask* t, TaskSyscallState& syscall_state) {
  for (RecordTask* child : t->emulated_ptrace_tracees) {
    if (t->is_waiting_for_ptrace(child) &&
        child->emulated_ptrace_stop_pending) {
      syscall_state.ptraced_tracee = child;
      return true;
    }
  }
  return false;
}

static void maybe_pause_instead_of_waiting(RecordTask* t) {
  if (t->in_wait_type != WAIT_TYPE_PID) {
    return;
  }
  RecordTask* child = t->session().find_task(t->in_wait_pid);
  if (!child || !t->is_waiting_for_ptrace(child) || t->is_waiting_for(child)) {
    return;
  }
  // OK, t is waiting for a ptrace child by tid, but since t is not really
  // ptracing child, entering a real wait syscall will not actually wait for
  // the child, so the kernel may error out with ECHILD (non-ptracers can't
  // wait on specific threads of another process, or for non-child processes).
  // To avoid this problem, we'll replace the wait syscall with a pause()
  // syscall.
  // It would be nice if we didn't have to do this, but I can't see a better
  // way.
  Registers r = t->regs();
  r.set_original_syscallno(syscall_number_for_pause(t->arch()));
  t->set_regs(r);
}

static RecordTask* verify_ptrace_target(RecordTask* tracer,
                                        TaskSyscallState& syscall_state,
                                        pid_t pid) {
  RecordTask* tracee = tracer->session().find_task(pid);
  if (!tracee || tracee->emulated_ptracer != tracer ||
      tracee->emulated_stop_type == NOT_STOPPED) {
    syscall_state.emulate_result(-ESRCH);
    return nullptr;
  }
  return tracee;
}

static void do_ptrace_exit_stop(RecordTask* t) {
  // Notify ptracer of the exit if it's not going to receive it from the
  // kernel because it's not the parent. (The kernel has similar logic to
  // deliver two stops in this case.)
  t->emulated_ptrace_queued_exit_stop = false;
  if (t->emulated_ptracer &&
      (t->is_clone_child() ||
       t->get_parent_pid() != t->emulated_ptracer->real_tgid())) {
    // This is a bit wrong; this is an exit stop, not a signal/ptrace stop.
    t->emulate_ptrace_stop(WaitStatus::for_exit_code(t->exit_code));
  }
}

static void prepare_ptrace_cont(RecordTask* tracee, int sig, int command) {
  if (sig) {
    siginfo_t si = tracee->take_ptrace_signal_siginfo(sig);
    // Treat signal as nondeterministic; it won't happen just by
    // replaying the tracee.
    tracee->push_event(SignalEvent(si, NONDETERMINISTIC_SIG, tracee));
  }

  tracee->emulated_stop_type = NOT_STOPPED;
  tracee->emulated_ptrace_stop_code = WaitStatus();
  tracee->emulated_ptrace_cont_command = command;

  if (tracee->ev().is_syscall_event() &&
      PROCESSING_SYSCALL == tracee->ev().Syscall().state) {
    // Continue the task since we didn't in enter_syscall
    tracee->resume_execution(RESUME_SYSCALL, RESUME_NONBLOCKING,
                             RESUME_NO_TICKS);
  }

  if (tracee->emulated_ptrace_queued_exit_stop) {
    do_ptrace_exit_stop(tracee);
  }
}

static uint64_t widen_buffer_unsigned(const void* buf, size_t size) {
  switch (size) {
    case 1:
      return *reinterpret_cast<const uint8_t*>(buf);
    case 2:
      return *reinterpret_cast<const uint16_t*>(buf);
    case 4:
      return *reinterpret_cast<const uint32_t*>(buf);
    case 8:
      return *reinterpret_cast<const uint64_t*>(buf);
    default:
      assert(0 && "Unsupported size");
      return 0;
  }
}

static int64_t widen_buffer_signed(const void* buf, size_t size) {
  switch (size) {
    case 1:
      return *reinterpret_cast<const int8_t*>(buf);
    case 2:
      return *reinterpret_cast<const int16_t*>(buf);
    case 4:
      return *reinterpret_cast<const int32_t*>(buf);
    case 8:
      return *reinterpret_cast<const int64_t*>(buf);
    default:
      assert(0 && "Unsupported size");
      return 0;
  }
}

static uint64_t path_inode_number(const char* path) {
  struct stat st;
  int ret = stat(path, &st);
  assert(ret == 0);
  return st.st_ino;
}

static bool is_same_namespace(const char* name, pid_t tid1, pid_t tid2) {
  char path1[PATH_MAX];
  char path2[PATH_MAX];
  sprintf(path1, "/proc/%d/ns/%s", tid1, name);
  sprintf(path2, "/proc/%d/ns/%s", tid2, name);
  return path_inode_number(path1) == path_inode_number(path2);
}

template <typename Arch>
static void ptrace_get_reg_set(RecordTask* t, TaskSyscallState& syscall_state,
                               const vector<uint8_t>& regs) {
  auto piov = syscall_state.reg_parameter<typename Arch::iovec>(4, IN_OUT);
  auto iov = t->read_mem(piov);
  iov.iov_len = min<size_t>(iov.iov_len, regs.size());
  t->write_mem(piov, iov);
  auto data = syscall_state.mem_ptr_parameter(REMOTE_PTR_FIELD(piov, iov_base),
                                              iov.iov_len);
  t->write_bytes_helper(data, iov.iov_len, regs.data());
  syscall_state.emulate_result(0);
}

template <typename Arch>
static void ptrace_verify_set_reg_set(RecordTask* t, size_t min_size,
                                      TaskSyscallState& syscall_state) {
  auto iov = t->read_mem(remote_ptr<typename Arch::iovec>(t->regs().arg4()));
  if (iov.iov_len < min_size) {
    syscall_state.emulate_result(-EIO);
  }
  syscall_state.emulate_result(0);
}

static bool verify_ptrace_options(RecordTask* t,
                                  TaskSyscallState& syscall_state) {
  // We "support" PTRACE_O_SYSGOOD because we don't support PTRACE_SYSCALL yet
  static const int supported_ptrace_options =
      PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEEXIT | PTRACE_O_TRACEFORK |
      PTRACE_O_TRACECLONE;

  if ((int)t->regs().arg4() & ~supported_ptrace_options) {
    LOG(debug) << "Unsupported ptrace options " << HEX(t->regs().arg4());
    syscall_state.emulate_result(-EINVAL);
    return false;
  }
  return true;
}

static RecordTask* prepare_ptrace_attach(RecordTask* t, pid_t pid,
                                         TaskSyscallState& syscall_state) {
  // To simplify things, require that a ptracer be in the same pid
  // namespace as rr itself. I.e., tracee tasks sandboxed in a pid
  // namespace can't use ptrace. This is normally a requirement of
  // sandboxes anyway.
  // This could be supported, but would require some work to translate
  // rr's pids to/from the ptracer's pid namespace.
  ASSERT(t, is_same_namespace("pid", t->tid, getpid()));
  RecordTask* tracee = t->session().find_task(pid);
  if (!tracee) {
    // XXX This prevents a tracee from attaching to a process which isn't
    // under rr's control. We could support this but it would complicate
    // things.
    syscall_state.emulate_result(-ESRCH);
    return nullptr;
  }
  // Don't allow a 32-bit process to trace a 64-bit process. That doesn't
  // make much sense (manipulating registers gets crazy), and would be hard to
  // support.
  if (tracee->emulated_ptracer || tracee->tgid() == t->tgid() ||
      (t->arch() == x86 && tracee->arch() == x86_64)) {
    syscall_state.emulate_result(-EPERM);
    return nullptr;
  }
  return tracee;
}

static void ptrace_attach_to_already_stopped_task(RecordTask* t) {
  ASSERT(t, t->emulated_stop_type == GROUP_STOP);
  // tracee is already stopped because of a group-stop signal.
  // Sending a SIGSTOP won't work, but we don't need to.
  t->force_emulate_ptrace_stop(WaitStatus::for_stop_sig(SIGSTOP));
  siginfo_t si;
  memset(&si, 0, sizeof(si));
  si.si_signo = SIGSTOP;
  si.si_code = SI_USER;
  t->save_ptrace_signal_siginfo(si);
}

template <typename Arch>
static Switchable prepare_ptrace(RecordTask* t,
                                 TaskSyscallState& syscall_state) {
  pid_t pid = (pid_t)t->regs().arg2_signed();
  bool emulate = true;
  int command = (int)t->regs().arg1_signed();
  switch (command) {
    case PTRACE_ATTACH: {
      RecordTask* tracee = prepare_ptrace_attach(t, pid, syscall_state);
      if (!tracee) {
        break;
      }
      tracee->set_emulated_ptracer(t);
      tracee->emulated_ptrace_seized = false;
      tracee->emulated_ptrace_options = 0;
      syscall_state.emulate_result(0);
      if (tracee->emulated_stop_type == NOT_STOPPED) {
        // Send SIGSTOP to this specific thread. Otherwise the kernel might
        // deliver SIGSTOP to some other thread of the process, and we won't
        // generate any ptrace event if that thread isn't being ptraced.
        tracee->tgkill(SIGSTOP);
      } else {
        ptrace_attach_to_already_stopped_task(tracee);
      }
      break;
    }
    case PTRACE_SEIZE: {
      RecordTask* tracee = prepare_ptrace_attach(t, pid, syscall_state);
      if (!tracee) {
        break;
      }
      if (t->regs().arg3()) {
        syscall_state.emulate_result(-EIO);
        break;
      }
      if (!verify_ptrace_options(t, syscall_state)) {
        break;
      }
      tracee->set_emulated_ptracer(t);
      tracee->emulated_ptrace_seized = true;
      tracee->emulated_ptrace_options = (int)t->regs().arg4();
      if (tracee->emulated_stop_type == GROUP_STOP) {
        ptrace_attach_to_already_stopped_task(tracee);
      }
      syscall_state.emulate_result(0);
      break;
    }
    case PTRACE_SETOPTIONS: {
      RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
      if (tracee) {
        if (!verify_ptrace_options(t, syscall_state)) {
          break;
        }
        tracee->emulated_ptrace_options = (int)t->regs().arg4();
        syscall_state.emulate_result(0);
      }
      break;
    }
    case PTRACE_GETEVENTMSG: {
      RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
      if (tracee) {
        auto datap =
            syscall_state.reg_parameter<typename Arch::unsigned_long>(4);
        t->write_mem(
            datap,
            (typename Arch::unsigned_long)tracee->emulated_ptrace_event_msg);
        syscall_state.emulate_result(0);
      }
      break;
    }
    case PTRACE_GETSIGINFO: {
      RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
      if (tracee) {
        auto datap = syscall_state.reg_parameter<typename Arch::siginfo_t>(4);
        typename Arch::siginfo_t dest;
        memset(&dest, 0, sizeof(dest));
        set_arch_siginfo(tracee->get_saved_ptrace_siginfo(), Arch::arch(),
                         &dest, sizeof(dest));
        t->write_mem(datap, dest);
        syscall_state.emulate_result(0);
      }
      break;
    }
    case PTRACE_GETREGS: {
      RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
      if (tracee) {
        auto data =
            syscall_state.reg_parameter<typename Arch::user_regs_struct>(4);
        auto regs = tracee->regs().get_ptrace_for_arch(Arch::arch());
        ASSERT(t, regs.size() == data.referent_size());
        t->write_bytes_helper(data, regs.size(), regs.data());
        syscall_state.emulate_result(0);
      }
      break;
    }
    case PTRACE_GETFPREGS: {
      RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
      if (tracee) {
        auto data =
            syscall_state.reg_parameter<typename Arch::user_fpregs_struct>(4);
        auto regs = tracee->extra_regs().get_user_fpregs_struct(Arch::arch());
        ASSERT(t, regs.size() == data.referent_size());
        t->write_bytes_helper(data, regs.size(), regs.data());
        syscall_state.emulate_result(0);
      }
      break;
    }
    case PTRACE_GETFPXREGS: {
      if (Arch::arch() != x86) {
        // GETFPXREGS is x86-32 only
        syscall_state.expect_errno = EIO;
        break;
      }
      RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
      if (tracee) {
        auto data =
            syscall_state.reg_parameter<X86Arch::user_fpxregs_struct>(4);
        auto regs = tracee->extra_regs().get_user_fpxregs_struct();
        t->write_mem(data, regs);
        syscall_state.emulate_result(0);
      }
      break;
    }
    case PTRACE_GETREGSET: {
      switch ((int)t->regs().arg3()) {
        case NT_PRSTATUS: {
          RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
          if (tracee) {
            auto regs = tracee->regs().get_ptrace_for_arch(Arch::arch());
            ptrace_get_reg_set<Arch>(t, syscall_state, regs);
          }
          break;
        }
        case NT_FPREGSET: {
          RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
          if (tracee) {
            auto regs =
                tracee->extra_regs().get_user_fpregs_struct(Arch::arch());
            ptrace_get_reg_set<Arch>(t, syscall_state, regs);
          }
          break;
        }
        case NT_X86_XSTATE: {
          RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
          if (tracee) {
            switch (tracee->extra_regs().format()) {
              case ExtraRegisters::XSAVE:
                ptrace_get_reg_set<Arch>(t, syscall_state,
                                         tracee->extra_regs().data());
                break;
              default:
                syscall_state.emulate_result(EINVAL);
                break;
            }
          }
          break;
        }
        default:
          syscall_state.expect_errno = EINVAL;
          emulate = false;
          break;
      }
      break;
    }
    case PTRACE_SETREGS: {
      RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
      if (tracee) {
        // The actual register effects are performed by
        // Task::on_syscall_exit_arch
        syscall_state.emulate_result(0);
      }
      break;
    }
    case PTRACE_SETFPREGS: {
      RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
      if (tracee) {
        // The actual register effects are performed by
        // Task::on_syscall_exit_arch
        syscall_state.emulate_result(0);
      }
      break;
    }
    case PTRACE_SETFPXREGS: {
      if (Arch::arch() != x86) {
        // SETFPXREGS is x86-32 only
        syscall_state.expect_errno = EIO;
        break;
      }
      RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
      if (tracee) {
        // The actual register effects are performed by
        // Task::on_syscall_exit_arch
        syscall_state.emulate_result(0);
      }
      break;
    }
    case PTRACE_SETREGSET: {
      // The actual register effects are performed by
      // Task::on_syscall_exit_arch
      switch ((int)t->regs().arg3()) {
        case NT_PRSTATUS: {
          RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
          if (tracee) {
            ptrace_verify_set_reg_set<Arch>(
                t, sizeof(typename Arch::user_regs_struct), syscall_state);
          }
          break;
        }
        case NT_FPREGSET: {
          RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
          if (tracee) {
            ptrace_verify_set_reg_set<Arch>(
                t, sizeof(typename Arch::user_fpregs_struct), syscall_state);
          }
          break;
        }
        case NT_X86_XSTATE: {
          RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
          if (tracee) {
            switch (tracee->extra_regs().format()) {
              case ExtraRegisters::XSAVE:
                ptrace_verify_set_reg_set<Arch>(
                    t, tracee->extra_regs().data_size(), syscall_state);
                break;
              default:
                syscall_state.emulate_result(EINVAL);
                break;
            }
          }
          break;
        }
        default:
          syscall_state.expect_errno = EINVAL;
          emulate = false;
          break;
      }
      break;
    }
    case PTRACE_PEEKTEXT:
    case PTRACE_PEEKDATA: {
      RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
      if (tracee) {
        // The actual syscall returns the data via the 'data' out-parameter.
        // The behavior of returning the data as the system call result is
        // provided by the glibc wrapper.
        auto datap =
            syscall_state.reg_parameter<typename Arch::unsigned_word>(4);
        remote_ptr<typename Arch::unsigned_word> addr = t->regs().arg3();
        bool ok = true;
        auto v = tracee->read_mem(addr, &ok);
        if (ok) {
          t->write_mem(datap, v);
          syscall_state.emulate_result(0);
        } else {
          syscall_state.emulate_result(-EIO);
        }
      }
      break;
    }
    case PTRACE_POKETEXT:
    case PTRACE_POKEDATA: {
      RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
      if (tracee) {
        remote_ptr<typename Arch::unsigned_word> addr = t->regs().arg3();
        typename Arch::unsigned_word data = t->regs().arg4();
        bool ok = true;
        tracee->write_mem(addr, data, &ok);
        if (ok) {
          // Since we're recording data that might not be for |t|, we have to
          // handle this specially during replay.
          tracee->record_local(addr, &data);
          syscall_state.emulate_result(0);
        } else {
          syscall_state.emulate_result(-EIO);
        }
      }
      break;
    }
    case PTRACE_PEEKUSER: {
      RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
      if (tracee) {
        // The actual syscall returns the data via the 'data' out-parameter.
        // The behavior of returning the data as the system call result is
        // provided by the glibc wrapper.
        size_t addr = t->regs().arg3();
        typename Arch::unsigned_word data;
        if ((addr & (sizeof(data) - 1)) ||
            addr >= sizeof(typename Arch::user)) {
          syscall_state.emulate_result(-EIO);
          break;
        }

        auto datap =
            syscall_state.reg_parameter<typename Arch::unsigned_word>(4);
        if (addr < sizeof(typename Arch::user_regs_struct)) {
          uint8_t buf[Registers::MAX_SIZE];
          bool defined;
          size_t size =
              tracee->regs().read_register_by_user_offset(buf, addr, &defined);
          if (defined) {
            // For unclear reasons, all 32-bit user_regs_struct members are
            // signed while all 64-bit user_regs_struct members are unsigned.
            if (Arch::arch() == x86) {
              data = widen_buffer_signed(buf, size);
            } else {
              data = widen_buffer_unsigned(buf, size);
            }
          } else {
            data = 0;
          }
        } else if (addr >= offsetof(typename Arch::user, u_debugreg[0]) &&
                   addr < offsetof(typename Arch::user, u_debugreg[8])) {
          size_t regno = (addr - offsetof(typename Arch::user, u_debugreg[0])) /
                         sizeof(data);
          data = tracee->get_debug_reg(regno);
        } else {
          data = 0;
        }

        t->write_mem(datap, data);
        syscall_state.emulate_result(0);
      }
      break;
    }
    case PTRACE_POKEUSER: {
      RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
      if (tracee) {
        // The actual syscall returns the data via the 'data' out-parameter.
        // The behavior of returning the data as the system call result is
        // provided by the glibc wrapper.
        size_t addr = t->regs().arg3();
        if ((addr & (sizeof(typename Arch::unsigned_word) - 1)) ||
            addr >= sizeof(typename Arch::user)) {
          syscall_state.emulate_result(-EIO);
          break;
        }
        syscall_state.emulate_result(0);
      }
      break;
    }
    case PTRACE_SYSCALL:
    case PTRACE_SINGLESTEP:
    case PTRACE_SYSEMU:
    case PTRACE_SYSEMU_SINGLESTEP:
    case PTRACE_CONT: {
      RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
      if (tracee) {
        prepare_ptrace_cont(tracee, t->regs().arg4(), command);
        syscall_state.emulate_result(0);
      }
      break;
    }
    case PTRACE_DETACH: {
      RecordTask* tracee = verify_ptrace_target(t, syscall_state, pid);
      if (tracee) {
        tracee->emulated_ptrace_options = 0;
        tracee->emulated_ptrace_cont_command = 0;
        tracee->emulated_ptrace_stop_pending = false;
        tracee->emulated_ptrace_queued_exit_stop = false;
        prepare_ptrace_cont(tracee, t->regs().arg4(), 0);
        tracee->set_emulated_ptracer(nullptr);
        syscall_state.emulate_result(0);
      }
      break;
    }
    default:
      syscall_state.expect_errno = EIO;
      emulate = false;
      break;
  }
  if (emulate) {
    Registers r = t->regs();
    r.set_arg1((intptr_t)-1);
    t->set_regs(r);
  }
  return PREVENT_SWITCH;
}

static void check_signals_while_exiting(RecordTask* t) {
  if (t->has_stashed_sig()) {
    // An unblockable signal (SIGKILL, SIGSTOP) might be received
    // and stashed. Since these signals are unblockable they take
    // effect no matter what and we don't need to deliver them to an exiting
    // thread.
    int sig = t->peek_stash_sig().siginfo.si_signo;
    ASSERT(t, sig == SIGKILL || sig == SIGSTOP);
  }
}

/**
 * At thread exit time, undo the work that init_buffers() did.
 *
 * Call this when the tracee has already entered SYS_exit/SYS_exit_group. The
 * tracee will be returned at a state in which it has entered (or
 * re-entered) SYS_exit/SYS_exit_group.
 */
static void prepare_exit(RecordTask* t, int exit_code) {
  t->stable_exit = true;
  t->exit_code = exit_code;
  t->session().scheduler().in_stable_exit(t);

  check_signals_while_exiting(t);

  Registers r = t->regs();
  Registers exit_regs = r;
  ASSERT(t,
         is_exit_syscall(exit_regs.original_syscallno(), t->arch()) ||
             is_exit_group_syscall(exit_regs.original_syscallno(), t->arch()))
      << "Tracee should have been at exit/exit_group, but instead at "
      << t->syscall_name(exit_regs.original_syscallno());

  // The first thing we need to do is to block all signals to prevent
  // a signal being delivered to the thread (since it's going to exit and
  // won't be able to handle any more signals).
  //
  // The tracee is at the entry to SYS_exit/SYS_exit_group, but hasn't started
  // the call yet.  We can't directly start injecting syscalls
  // because the tracee is still in the kernel.  And obviously,
  // if we finish the SYS_exit/SYS_exit_group syscall, the tracee isn't around
  // anymore.
  //
  // So hijack this SYS_exit call and rewrite it into a SYS_rt_sigprocmask.
  r.set_original_syscallno(syscall_number_for_rt_sigprocmask(t->arch()));
  r.set_arg1(SIG_BLOCK);
  r.set_arg2(AddressSpace::rr_page_ff_bytes());
  r.set_arg3(0);
  r.set_arg4(sizeof(sig_set_t));
  t->set_regs(r);
  // This exits the SYS_rt_sigprocmask.  Now the tracee is ready to do our
  // bidding.
  t->advance_syscall();
  check_signals_while_exiting(t);

  // Do the actual buffer and fd cleanup.
  t->destroy_buffers();
  check_signals_while_exiting(t);

  // Restore these regs to what they would have been just before
  // the tracee trapped at SYS_exit/SYS_exit_group.  When we've finished
  // cleanup, we'll restart the call.
  exit_regs.set_syscallno(exit_regs.original_syscallno());
  exit_regs.set_original_syscallno(-1);
  exit_regs.set_ip(exit_regs.ip() - syscall_instruction_length(t->arch()));
  ASSERT(t, is_at_syscall_instruction(t, exit_regs.ip()))
      << "Tracee should have entered through int $0x80.";
  // Restart the SYS_exit call.
  t->set_regs(exit_regs);
  t->advance_syscall();
  check_signals_while_exiting(t);

  if (t->emulated_ptrace_options & PTRACE_O_TRACEEXIT) {
    // Ensure that do_ptrace_exit_stop can run later.
    t->emulated_ptrace_queued_exit_stop = true;
    t->emulate_ptrace_stop(WaitStatus::for_ptrace_event(PTRACE_EVENT_EXIT));
  } else {
    // Only allow one stop at a time. After the PTRACE_EVENT_EXIT has been
    // processed, PTRACE_CONT will call do_ptrace_exit_stop for us.
    do_ptrace_exit_stop(t);
  }
}

static void prepare_mmap_register_params(RecordTask* t) {
  Registers r = t->regs();
  if (t->session().enable_chaos() &&
      !(r.arg4_signed() & (MAP_FIXED | MAP_32BIT)) && r.arg1() == 0) {
    // No address hint was provided. Randomize the allocation address.
    size_t len = r.arg2();
    if (r.arg4_signed() & MAP_GROWSDOWN) {
      // Ensure stacks can grow to the minimum size we choose
      len = max<size_t>(AddressSpace::chaos_mode_min_stack_size(), len);
    }
    remote_ptr<void> addr = t->vm()->chaos_mode_find_free_memory(t, len);
    if (!addr.is_null()) {
      r.set_arg1(addr + len - r.arg2());
      // Note that we don't set MAP_FIXED here. If anything goes wrong (e.g.
      // we pick a hint address that actually can't be used on this system), the
      // kernel will pick a valid address instead.
    }
  }
  r.set_arg4(r.arg4_signed() & ~MAP_GROWSDOWN);
  t->set_regs(r);
}

static void set_own_namespace_tid(RecordTask* t) {
  AutoRemoteSyscalls remote(t);
  t->own_namespace_rec_tid =
      remote.infallible_syscall(syscall_number_for_gettid(t->arch()));
}

enum ScratchAddrType { FIXED_ADDRESS, DYNAMIC_ADDRESS };
/* Pointer used when running RR in WINE. Memory below this address is
   unmapped by WINE immediately after exec, so start the scratch buffer
   here. */
static const uintptr_t FIXED_SCRATCH_PTR = 0x68000000;

static void init_scratch_memory(RecordTask* t,
                                ScratchAddrType addr_type = DYNAMIC_ADDRESS) {
  const int scratch_size = 512 * page_size();
  size_t sz = scratch_size;
  // The PROT_EXEC looks scary, and it is, but it's to prevent
  // this region from being coalesced with another anonymous
  // segment mapped just after this one.  If we named this
  // segment, we could remove this hack.
  int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  {
    /* initialize the scratchpad for blocking system calls */
    AutoRemoteSyscalls remote(t);

    if (addr_type == DYNAMIC_ADDRESS) {
      t->scratch_ptr = remote.infallible_mmap_syscall(remote_ptr<void>(), sz,
                                                      prot, flags, -1, 0);
    } else {
      t->scratch_ptr =
          remote.infallible_mmap_syscall(remote_ptr<void>(FIXED_SCRATCH_PTR),
                                         sz, prot, flags | MAP_FIXED, -1, 0);
    }
    t->scratch_size = scratch_size;
  }
  // record this mmap for the replay
  Registers r = t->regs();
  uintptr_t saved_result = r.syscall_result();
  r.set_syscall_result(t->scratch_ptr);
  t->set_regs(r);

  KernelMapping km =
      t->vm()->map(t->scratch_ptr, sz, prot, flags, 0, string(),
                   KernelMapping::NO_DEVICE, KernelMapping::NO_INODE);
  struct stat stat;
  memset(&stat, 0, sizeof(stat));
  auto record_in_trace = t->trace_writer().write_mapped_region(t, km, stat);
  ASSERT(t, record_in_trace == TraceWriter::DONT_RECORD_IN_TRACE);

  r.set_syscall_result(saved_result);
  t->set_regs(r);
}

static int ptrace_option_for_event(int ptrace_event) {
  switch (ptrace_event) {
    case PTRACE_EVENT_FORK:
      return PTRACE_O_TRACEFORK;
    case PTRACE_EVENT_CLONE:
      return PTRACE_O_TRACECLONE;
    case PTRACE_EVENT_VFORK:
      return PTRACE_O_TRACEVFORK;
    default:
      FATAL() << "Unsupported ptrace event";
      return 0;
  }
}

template <typename Arch>
static void prepare_clone(RecordTask* t, TaskSyscallState& syscall_state) {
  uintptr_t flags = 0;
  CloneParameters params;
  Registers r = t->regs();
  int original_syscall = r.original_syscallno();
  int ptrace_event;
  int termination_signal = SIGCHLD;

  if (is_clone_syscall(original_syscall, r.arch())) {
    params = extract_clone_parameters(t);
    flags = r.arg1();
    r.set_arg1(flags & ~uintptr_t(CLONE_VFORK | CLONE_UNTRACED));
    t->set_regs(r);
    termination_signal = flags & 0xff;
    if (flags & CLONE_VFORK) {
      ptrace_event = PTRACE_EVENT_VFORK;
    } else if (termination_signal == SIGCHLD) {
      ptrace_event = PTRACE_EVENT_FORK;
    } else {
      ptrace_event = PTRACE_EVENT_CLONE;
    }
  } else if (is_vfork_syscall(original_syscall, r.arch())) {
    r.set_original_syscallno(Arch::fork);
    t->set_regs(r);
    ptrace_event = PTRACE_EVENT_VFORK;
  } else {
    ptrace_event = PTRACE_EVENT_FORK;
  }

  while (true) {
    t->resume_execution(RESUME_SYSCALL, RESUME_WAIT, RESUME_NO_TICKS);
    if (t->ptrace_event()) {
      break;
    }
    ASSERT(t, !t->stop_sig());
    ASSERT(t, t->regs().syscall_result_signed() < 0);
    if (!t->regs().syscall_may_restart()) {
      syscall_state.emulate_result(t->regs().syscall_result());
      // clone failed and we're existing the syscall with an error. Reenter
      // the syscall so that we're in the same state as the normal execution
      // path.
      t->ev().Syscall().failed_during_preparation = true;
      r.set_syscallno(Arch::gettid);
      r.set_ip(r.ip().decrement_by_syscall_insn_length(r.arch()));
      t->set_regs(r);
      t->advance_syscall();
      r.set_ip(t->regs().ip());
      r.set_original_syscallno(original_syscall);
      t->set_regs(r);
      return;
    }
    // Reenter the syscall. If we try to return an ERESTART* error using the
    // code path above, our set_syscallno(SYS_gettid) fails to take effect and
    // we actually do the clone, and things get horribly confused.
    r.set_syscallno(r.original_syscallno());
    r.set_ip(r.ip().decrement_by_syscall_insn_length(r.arch()));
    t->set_regs(r);
    t->advance_syscall();
  }

  ASSERT(t, t->ptrace_event() == PTRACE_EVENT_CLONE ||
                t->ptrace_event() == PTRACE_EVENT_FORK);

  // Ideally we'd just use t->get_ptrace_eventmsg_pid() here, but
  // kernels failed to translate that value from other pid namespaces to
  // our pid namespace until June 2014:
  // https://github.com/torvalds/linux/commit/4e52365f279564cef0ddd41db5237f0471381093
  pid_t new_tid;
  if (flags & CLONE_THREAD) {
    new_tid = t->find_newborn_thread();
  } else {
    new_tid = t->find_newborn_child_process();
  }
  RecordTask* new_task = static_cast<RecordTask*>(
      t->session().clone(t, clone_flags_to_task_flags(flags), params.stack,
                         params.tls, params.ctid, new_tid));
  set_own_namespace_tid(new_task);

  // Restore modified registers in cloned task
  Registers new_r = new_task->regs();
  new_r.set_original_syscallno(
      syscall_state.syscall_entry_registers.original_syscallno());
  new_r.set_arg1(syscall_state.syscall_entry_registers.arg1());
  new_task->emulate_syscall_entry(new_r);
  new_task->set_termination_signal(termination_signal);

  /* record child id here */
  if (is_clone_syscall(r.original_syscallno(), r.arch())) {
    CloneParameters child_params = extract_clone_parameters(new_task);
    t->record_remote_even_if_null(params.ptid);

    if (Arch::clone_tls_type == Arch::UserDescPointer) {
      t->record_remote_even_if_null(
          params.tls.cast<typename Arch::user_desc>());
      new_task->record_remote_even_if_null(
          child_params.tls.cast<typename Arch::user_desc>());
    } else {
      assert(Arch::clone_tls_type == Arch::PthreadStructurePointer);
    }
    new_task->record_remote_even_if_null(child_params.ptid);
    new_task->record_remote_even_if_null(child_params.ctid);

    t->session().trace_writer().write_task_event(
        TraceTaskEvent::for_clone(new_task->tid, t->tid, flags));
  } else {
    t->session().trace_writer().write_task_event(
        TraceTaskEvent::for_fork(new_task->tid, t->tid));
  }

  init_scratch_memory(new_task);

  if ((t->emulated_ptrace_options & ptrace_option_for_event(ptrace_event)) &&
      !(flags & CLONE_UNTRACED)) {
    new_task->set_emulated_ptracer(t->emulated_ptracer);
    new_task->emulated_ptrace_seized = t->emulated_ptrace_seized;
    new_task->emulated_ptrace_options = t->emulated_ptrace_options;
    t->emulated_ptrace_event_msg = new_task->rec_tid;
    t->emulate_ptrace_stop(WaitStatus::for_ptrace_event(ptrace_event));
    // ptrace(2) man page says that SIGSTOP is used here, but it's really
    // SIGTRAP (in 4.4.4-301.fc23.x86_64 anyway).
    new_task->apply_group_stop(SIGTRAP);
  }

  // Restore our register modifications now, so that the emulated ptracer will
  // see the original registers without our modifications if it inspects them
  // in the ptrace event.
  r = t->regs();
  r.set_arg1(syscall_state.syscall_entry_registers.arg1());
  r.set_original_syscallno(
      syscall_state.syscall_entry_registers.original_syscallno());
  t->set_regs(r);

  // We're in a PTRACE_EVENT_FORK/CLONE so the next PTRACE_SYSCALL for |t| will
  // go to the exit of the syscall, as expected.
}

static void record_ranges(RecordTask* t,
                          const vector<FileMonitor::Range>& ranges,
                          size_t size) {
  size_t s = size;
  for (auto& r : ranges) {
    size_t bytes = min(s, r.length);
    if (bytes > 0) {
      t->record_remote(r.data, bytes);
      s -= bytes;
    }
  }
}

template <typename Arch>
static Switchable rec_prepare_syscall_arch(RecordTask* t,
                                           TaskSyscallState& syscall_state) {
  int syscallno = t->ev().Syscall().number;

  syscall_state.syscall_entry_registers = t->regs();

  if (t->desched_rec()) {
    /* |t| was descheduled while in a buffered syscall.  We normally don't
     * use scratch memory for the call, because the syscallbuf itself
     * is serving that purpose. More importantly, we *can't* set up
     * scratch for |t|, because it's already in the syscall. Instead, we will
     * record the syscallbuf memory in rec_process_syscall_arch.
     *
     * However there is one case where we use scratch memory: when
     * sys_read's block-cloning path is interrupted. In that case, record
     * the scratch memory.
     */
    if (syscallno == Arch::read &&
        remote_ptr<void>(t->regs().arg2()) == t->scratch_ptr) {
      syscall_state.reg_parameter(
          2, ParamSize::from_syscall_result<typename Arch::ssize_t>(
                 (size_t)t->regs().arg3()),
          IN_OUT_NO_SCRATCH);
    }
    return ALLOW_SWITCH;
  }

  if (syscallno < 0) {
    // Invalid syscall. Don't let it accidentally match a
    // syscall number below that's for an undefined syscall.
    syscall_state.expect_errno = ENOSYS;
    return PREVENT_SWITCH;
  }

  switch (syscallno) {
// All the regular syscalls are handled here.
#include "SyscallRecordCase.generated"

    case Arch::splice: {
      syscall_state.reg_parameter<loff_t>(2, IN_OUT);
      syscall_state.reg_parameter<loff_t>(4, IN_OUT);
      return ALLOW_SWITCH;
    }

    case Arch::sendfile: {
      syscall_state.reg_parameter<typename Arch::off_t>(3, IN_OUT);
      return ALLOW_SWITCH;
    }
    case Arch::sendfile64: {
      syscall_state.reg_parameter<typename Arch::off64_t>(3, IN_OUT);
      return ALLOW_SWITCH;
    }

    case Arch::capget: {
      auto hdr = t->read_mem(
          syscall_state.reg_parameter<typename Arch::__user_cap_header_struct>(
              1, IN_OUT));
      int struct_count;
      switch (hdr.version) {
        case _LINUX_CAPABILITY_VERSION_1:
          struct_count = _LINUX_CAPABILITY_U32S_1;
          break;
        case _LINUX_CAPABILITY_VERSION_2:
          struct_count = _LINUX_CAPABILITY_U32S_2;
          break;
        case _LINUX_CAPABILITY_VERSION_3:
          struct_count = _LINUX_CAPABILITY_U32S_3;
          break;
        default:
          struct_count = 0;
          break;
      }
      if (struct_count > 0) {
        syscall_state.reg_parameter(
            2, sizeof(typename Arch::__user_cap_data_struct) * struct_count,
            OUT);
      }
      return PREVENT_SWITCH;
    }

    case Arch::fork:
    case Arch::vfork:
    case Arch::clone:
      prepare_clone<Arch>(t, syscall_state);
      return ALLOW_SWITCH;

    case Arch::exit:
      if (t->task_group()->task_set().size() == 1) {
        t->task_group()->exit_code = (int)t->regs().arg1();
      }
      prepare_exit(t, (int)t->regs().arg1());
      return ALLOW_SWITCH;

    case Arch::exit_group:
      t->task_group()->exit_code = (int)t->regs().arg1();
      if (t->task_group()->task_set().size() == 1) {
        prepare_exit(t, (int)t->regs().arg1());
        return ALLOW_SWITCH;
      }
      return PREVENT_SWITCH;

    case Arch::execve: {
      vector<string> cmd_line;
      remote_ptr<typename Arch::unsigned_word> argv = t->regs().arg2();
      while (true) {
        auto p = t->read_mem(argv);
        if (!p) {
          break;
        }
        cmd_line.push_back(t->read_c_str(p));
        argv++;
      }

      // Save the event. We can't record it here because the exec might fail.
      string raw_filename = t->read_c_str(t->regs().arg1());
      syscall_state.exec_saved_event =
          unique_ptr<TraceTaskEvent>(new TraceTaskEvent(
              TraceTaskEvent::for_exec(t->tid, raw_filename, cmd_line)));

      return PREVENT_SWITCH;
    }

    case Arch::fcntl:
    case Arch::fcntl64: {
      int fd = t->regs().arg1();
      uint64_t result;
      if (t->fd_table()->emulate_fcntl(fd, t, &result)) {
        // Don't perform this syscall.
        Registers r = t->regs();
        r.set_arg1(-1);
        t->set_regs(r);
        syscall_state.emulate_result(result);
        return PREVENT_SWITCH;
      }
      switch ((int)t->regs().arg2_signed()) {
        case Arch::DUPFD:
        case Arch::DUPFD_CLOEXEC:
        case Arch::GETFD:
        case Arch::GETFL:
        case Arch::SETFL:
        case Arch::SETLK:
        case Arch::SETLK64:
        case Arch::SETOWN:
        case Arch::SETOWN_EX:
        case Arch::GETSIG:
        case Arch::SETSIG:
        case Arch::ADD_SEALS:
          break;

        case Arch::SETFD:
          if (!t->fd_table()->allow_close(fd)) {
            // Don't let tracee set FD_CLOEXEC on this fd. Disable the syscall,
            // but emulate a successful return.
            Registers r = t->regs();
            r.set_arg1(-1);
            t->set_regs(r);
            syscall_state.emulate_result(0);
          }
          break;

        case Arch::GETLK:
          syscall_state.reg_parameter<struct Arch::flock>(3, IN_OUT);
          break;

        case Arch::GETLK64:
          // flock and flock64 better be different on 32-bit architectures,
          // but on 64-bit architectures, it's OK if they're the same.
          static_assert(
              sizeof(struct Arch::flock) < sizeof(typename Arch::flock64) ||
                  Arch::elfclass == ELFCLASS64,
              "struct flock64 not declared differently from struct flock");
          syscall_state.reg_parameter<typename Arch::flock64>(3, IN_OUT);
          break;

        case Arch::GETOWN_EX:
          syscall_state.reg_parameter<typename Arch::f_owner_ex>(3);
          break;

        case Arch::SETLKW:
        case Arch::SETLKW64:
          // SETLKW blocks, but doesn't write any
          // outparam data to the |struct flock|
          // argument, so no need for scratch.
          return ALLOW_SWITCH;

        default:
          // Unknown command should trigger EINVAL.
          syscall_state.expect_errno = EINVAL;
          break;
      }
      return PREVENT_SWITCH;
    }

    /* int futex(int *uaddr, int op, int val, const struct timespec *timeout,
     *           int *uaddr2, int val3);
     * futex parameters are in-out but they can't be moved to scratch
     * addresses. */
    case Arch::futex: {
      int op = t->regs().arg2_signed();
      switch (op & FUTEX_CMD_MASK) {
        case FUTEX_WAIT:
        case FUTEX_WAIT_BITSET:
          return ALLOW_SWITCH;

        case FUTEX_CMP_REQUEUE:
        case FUTEX_WAKE_OP:
          syscall_state.reg_parameter<int>(5, IN_OUT_NO_SCRATCH);
          break;

        case FUTEX_WAKE:
          break;

        default:
          syscall_state.expect_errno = EINVAL;
          break;
      }
      return PREVENT_SWITCH;
    }

    case Arch::getrandom:
      syscall_state.reg_parameter(
          1, ParamSize::from_syscall_result<int>((size_t)t->regs().arg2()));
      return (GRND_NONBLOCK & t->regs().arg3()) ? PREVENT_SWITCH : ALLOW_SWITCH;

    case Arch::set_thread_area:
      syscall_state.reg_parameter<typename Arch::user_desc>(1, IN_OUT);
      return PREVENT_SWITCH;

    case Arch::ipc:
      switch ((int)t->regs().arg1_signed()) {
        case MSGGET:
        case SHMDT:
        case SHMGET:
        case SEMGET:
          break;

        case MSGCTL: {
          int cmd = (int)t->regs().arg3_signed() & ~IPC_64;
          return prepare_msgctl<Arch>(syscall_state, cmd, 5);
        }

        case MSGSND:
        case SEMOP:
        case SEMTIMEDOP:
          return ALLOW_SWITCH;

        case MSGRCV: {
          size_t msgsize = t->regs().arg3();
          auto kluge_args =
              syscall_state.reg_parameter<typename Arch::ipc_kludge_args>(5,
                                                                          IN);
          syscall_state.mem_ptr_parameter(REMOTE_PTR_FIELD(kluge_args, msgbuf),
                                          sizeof(typename Arch::signed_long) +
                                              msgsize);
          return ALLOW_SWITCH;
        }

        case SHMAT: {
          // Insane legacy feature: ipc SHMAT returns its pointer via an
          // in-memory out parameter.
          syscall_state.reg_parameter<typename Arch::unsigned_long>(4);
          return PREVENT_SWITCH;
        }

        case SHMCTL: {
          int cmd = (int)t->regs().arg3_signed() & ~IPC_64;
          return prepare_shmctl<Arch>(syscall_state, cmd, 5);
        }

        case SEMCTL: {
          int cmd = (int)t->regs().arg4_signed() & ~IPC_64;
          return prepare_semctl<Arch>(t, syscall_state,
                                      (int)t->regs().arg2_signed(), cmd, 5,
                                      DEREFERENCE);
        }

        default:
          syscall_state.expect_errno = EINVAL;
          break;
      }
      return PREVENT_SWITCH;

    case Arch::msgctl:
      return prepare_msgctl<Arch>(syscall_state, (int)t->regs().arg2_signed(),
                                  3);

    case Arch::msgrcv: {
      size_t msgsize = t->regs().arg3();
      syscall_state.reg_parameter(2,
                                  sizeof(typename Arch::signed_long) + msgsize);
      return ALLOW_SWITCH;
    }

    case Arch::msgsnd:
    case Arch::semop:
    case Arch::semtimedop:
      return ALLOW_SWITCH;

    case Arch::socketcall:
      return prepare_socketcall<Arch>(t, syscall_state);

    case Arch::select:
    case Arch::_newselect:
      if (syscallno == Arch::select &&
          Arch::select_semantics == Arch::SelectStructArguments) {
        auto argsp =
            syscall_state.reg_parameter<typename Arch::select_args>(1, IN);
        syscall_state.mem_ptr_parameter_inferred(
            REMOTE_PTR_FIELD(argsp, read_fds), IN_OUT);
        syscall_state.mem_ptr_parameter_inferred(
            REMOTE_PTR_FIELD(argsp, write_fds), IN_OUT);
        syscall_state.mem_ptr_parameter_inferred(
            REMOTE_PTR_FIELD(argsp, except_fds), IN_OUT);
        syscall_state.mem_ptr_parameter_inferred(
            REMOTE_PTR_FIELD(argsp, timeout), IN_OUT);
      } else {
        syscall_state.reg_parameter<typename Arch::fd_set>(2, IN_OUT);
        syscall_state.reg_parameter<typename Arch::fd_set>(3, IN_OUT);
        syscall_state.reg_parameter<typename Arch::fd_set>(4, IN_OUT);
        syscall_state.reg_parameter<typename Arch::timeval>(5, IN_OUT);
      }
      return ALLOW_SWITCH;

    case Arch::pselect6:
      syscall_state.reg_parameter<typename Arch::fd_set>(2, IN_OUT);
      syscall_state.reg_parameter<typename Arch::fd_set>(3, IN_OUT);
      syscall_state.reg_parameter<typename Arch::fd_set>(4, IN_OUT);
      syscall_state.reg_parameter<typename Arch::timespec>(5, IN_OUT);
      return ALLOW_SWITCH;

    case Arch::recvfrom: {
      syscall_state.reg_parameter(
          2, ParamSize::from_syscall_result<typename Arch::ssize_t>(
                 t->regs().arg3()));
      auto addrlen_ptr =
          syscall_state.reg_parameter<typename Arch::socklen_t>(6, IN_OUT);
      syscall_state.reg_parameter(
          5, ParamSize::from_initialized_mem(t, addrlen_ptr));
      return ALLOW_SWITCH;
    }

    case Arch::recvmsg: {
      auto msgp = syscall_state.reg_parameter<typename Arch::msghdr>(2, IN_OUT);
      prepare_recvmsg<Arch>(
          t, syscall_state, msgp,
          ParamSize::from_syscall_result<typename Arch::ssize_t>());
      if (!((int)t->regs().arg3() & MSG_DONTWAIT)) {
        return ALLOW_SWITCH;
      }
      return PREVENT_SWITCH;
    }

    case Arch::recvmmsg: {
      auto vlen = (unsigned int)t->regs().arg3();
      auto mmsgp =
          syscall_state.reg_parameter(2, sizeof(typename Arch::mmsghdr) * vlen,
                                      IN_OUT)
              .cast<typename Arch::mmsghdr>();
      prepare_recvmmsg<Arch>(t, syscall_state, mmsgp, vlen);
      if (!((unsigned int)t->regs().arg4() & MSG_DONTWAIT)) {
        return ALLOW_SWITCH;
      }
      return PREVENT_SWITCH;
    }

    case Arch::sendmsg:
      if (!((unsigned int)t->regs().arg3() & MSG_DONTWAIT)) {
        return ALLOW_SWITCH;
      }
      return PREVENT_SWITCH;

    case Arch::sendmmsg: {
      auto vlen = (unsigned int)t->regs().arg3();
      syscall_state.reg_parameter(2, sizeof(typename Arch::mmsghdr) * vlen,
                                  IN_OUT);
      if (!((unsigned int)t->regs().arg4() & MSG_DONTWAIT)) {
        return ALLOW_SWITCH;
      }
      return PREVENT_SWITCH;
    }

    case Arch::getsockname:
    case Arch::getpeername: {
      auto addrlen_ptr =
          syscall_state.reg_parameter<typename Arch::socklen_t>(3, IN_OUT);
      syscall_state.reg_parameter(
          2, ParamSize::from_initialized_mem(t, addrlen_ptr));
      return PREVENT_SWITCH;
    }

    case Arch::getsockopt: {
      auto optlen_ptr =
          syscall_state.reg_parameter<typename Arch::socklen_t>(5, IN_OUT);
      syscall_state.reg_parameter(
          4, ParamSize::from_initialized_mem(t, optlen_ptr));
      return PREVENT_SWITCH;
    }

    case Arch::pread64:
    /* ssize_t read(int fd, void *buf, size_t count); */
    case Arch::read: {
      int fd = t->regs().arg1();
      uint64_t result;
      vector<FileMonitor::Range> ranges;
      ranges.push_back(FileMonitor::Range(t->regs().arg2(), t->regs().arg3()));
      if (t->fd_table()->emulate_read(
              fd, t, ranges, t->get_io_offset(syscallno, t->regs()), &result)) {
        // Don't perform this syscall.
        Registers r = t->regs();
        r.set_arg1(-1);
        t->set_regs(r);
        record_ranges(t, ranges, result);
        syscall_state.emulate_result(result);
        return PREVENT_SWITCH;
      }
      syscall_state.reg_parameter(
          2, ParamSize::from_syscall_result<typename Arch::ssize_t>(
                 (size_t)t->regs().arg3()));
      return ALLOW_SWITCH;
    }

    case Arch::accept:
    case Arch::accept4: {
      auto addrlen_ptr =
          syscall_state.reg_parameter<typename Arch::socklen_t>(3, IN_OUT);
      syscall_state.reg_parameter(
          2, ParamSize::from_initialized_mem(t, addrlen_ptr));
      return ALLOW_SWITCH;
    }

    case Arch::getcwd: {
      syscall_state.reg_parameter(
          1, ParamSize::from_syscall_result<typename Arch::ssize_t>(
                 (size_t)t->regs().arg2()));
      return PREVENT_SWITCH;
    }

    case Arch::getdents:
    case Arch::getdents64: {
      syscall_state.reg_parameter(2, ParamSize::from_syscall_result<int>(
                                         (unsigned int)t->regs().arg3()));
      return PREVENT_SWITCH;
    }

    case Arch::readlink: {
      syscall_state.reg_parameter(
          2, ParamSize::from_syscall_result<typename Arch::ssize_t>(
                 (size_t)t->regs().arg3()));
      return PREVENT_SWITCH;
    }

    case Arch::readlinkat: {
      syscall_state.reg_parameter(
          3, ParamSize::from_syscall_result<typename Arch::ssize_t>(
                 (size_t)t->regs().arg4()));
      return PREVENT_SWITCH;
    }

    case Arch::getgroups: {
      // We could record a little less data by restricting the recorded data
      // to the syscall result * sizeof(Arch::legacy_gid_t), but that would
      // require more infrastructure and it's not worth worrying about.
      syscall_state.reg_parameter(2, (int)t->regs().arg1_signed() *
                                         sizeof(typename Arch::legacy_gid_t));
      return PREVENT_SWITCH;
    }

    case Arch::getgroups32: {
      // We could record a little less data by restricting the recorded data
      // to the syscall result * sizeof(Arch::gid_t), but that would
      // require more infrastructure and it's not worth worrying about.
      syscall_state.reg_parameter(2, (int)t->regs().arg1_signed() *
                                         sizeof(typename Arch::gid_t));
      return PREVENT_SWITCH;
    }

    case Arch::write:
    case Arch::writev: {
      int fd = (int)t->regs().arg1_signed();
      return t->fd_table()->will_write(t, fd);
    }

    /* ssize_t readv(int fd, const struct iovec *iov, int iovcnt); */
    case Arch::readv:
    /* ssize_t preadv(int fd, const struct iovec *iov, int iovcnt,
                      off_t offset); */
    case Arch::preadv: {
      int fd = (int)t->regs().arg1_signed();
      int iovcnt = (int)t->regs().arg3_signed();
      remote_ptr<void> iovecsp_void = syscall_state.reg_parameter(
          2, sizeof(typename Arch::iovec) * iovcnt, IN);
      auto iovecsp = iovecsp_void.cast<typename Arch::iovec>();
      auto iovecs = t->read_mem(iovecsp, iovcnt);
      uint64_t result;
      vector<FileMonitor::Range> ranges;
      for (int i = 0; i < iovcnt; ++i) {
        ranges.push_back(
            FileMonitor::Range(iovecs[i].iov_base, iovecs[i].iov_len));
      }
      if (t->fd_table()->emulate_read(
              fd, t, ranges, t->get_io_offset(syscallno, t->regs()), &result)) {
        // Don't perform this syscall.
        Registers r = t->regs();
        r.set_arg1(-1);
        t->set_regs(r);
        record_ranges(t, ranges, result);
        syscall_state.emulate_result(result);
        return PREVENT_SWITCH;
      }
      ParamSize io_size =
          ParamSize::from_syscall_result<typename Arch::ssize_t>();
      for (int i = 0; i < iovcnt; ++i) {
        syscall_state.mem_ptr_parameter(REMOTE_PTR_FIELD(iovecsp + i, iov_base),
                                        io_size.limit_size(iovecs[i].iov_len));
      }
      return ALLOW_SWITCH;
    }

    /* pid_t waitpid(pid_t pid, int *status, int options); */
    /* pid_t wait4(pid_t pid, int *status, int options, struct rusage
     * *rusage);
     */
    case Arch::waitpid:
    case Arch::wait4: {
      syscall_state.reg_parameter<int>(2, IN_OUT);
      if (syscallno == Arch::wait4) {
        syscall_state.reg_parameter<typename Arch::rusage>(4);
      }
      pid_t pid = (pid_t)t->regs().arg1_signed();
      if (pid < -1) {
        t->in_wait_type = WAIT_TYPE_PGID;
        t->in_wait_pid = -pid;
      } else if (pid == -1) {
        t->in_wait_type = WAIT_TYPE_ANY;
      } else if (pid == 0) {
        t->in_wait_type = WAIT_TYPE_SAME_PGID;
      } else {
        t->in_wait_type = WAIT_TYPE_PID;
        t->in_wait_pid = pid;
      }
      if (maybe_emulate_wait(t, syscall_state)) {
        Registers r = t->regs();
        // Set options to an invalid value to force syscall to fail
        r.set_arg3(0xffffffff);
        t->set_regs(r);
        return PREVENT_SWITCH;
      }
      maybe_pause_instead_of_waiting(t);
      return ALLOW_SWITCH;
    }

    case Arch::waitid: {
      syscall_state.reg_parameter<typename Arch::siginfo_t>(3, IN_OUT);
      t->in_wait_pid = (id_t)t->regs().arg2();
      switch ((idtype_t)t->regs().arg1()) {
        case P_ALL:
          t->in_wait_type = WAIT_TYPE_ANY;
          break;
        case P_PID:
          t->in_wait_type = WAIT_TYPE_PID;
          break;
        case P_PGID:
          t->in_wait_type = WAIT_TYPE_PGID;
          break;
        default:
          syscall_state.expect_errno = EINVAL;
          break;
      }
      if (maybe_emulate_wait(t, syscall_state)) {
        Registers r = t->regs();
        // Set options to an invalid value to force syscall to fail
        r.set_arg4(0xffffffff);
        t->set_regs(r);
        return PREVENT_SWITCH;
      }
      maybe_pause_instead_of_waiting(t);
      return ALLOW_SWITCH;
    }

    case Arch::setpriority:
      // The syscall might fail due to insufficient
      // permissions (e.g. while trying to decrease the nice value
      // while not root).
      // We'll choose to honor the new value anyway since we'd like
      // to be able to test configurations where a child thread
      // has a lower nice value than its parent, which requires
      // lowering the child's nice value.
      if ((int)t->regs().arg1_signed() == PRIO_PROCESS) {
        RecordTask* target =
            (int)t->regs().arg2_signed()
                ? t->session().find_task((int)t->regs().arg2_signed())
                : t;
        if (target) {
          LOG(debug) << "Setting nice value for tid " << t->tid << " to "
                     << t->regs().arg3();
          target->session().scheduler().update_task_priority(
              target, (int)t->regs().arg3_signed());
        }
      }
      return PREVENT_SWITCH;

    case Arch::pause:
      return ALLOW_SWITCH;

    /* int poll(struct pollfd *fds, nfds_t nfds, int timeout) */
    /* int ppoll(struct pollfd *fds, nfds_t nfds,
     *           const struct timespec *timeout_ts,
     *           const sigset_t *sigmask); */
    case Arch::poll:
    case Arch::ppoll: {
      auto nfds = (nfds_t)t->regs().arg2();
      syscall_state.reg_parameter(1, sizeof(typename Arch::pollfd) * nfds,
                                  IN_OUT);
      return ALLOW_SWITCH;
    }

    case Arch::perf_event_open: {
      RecordTask* target =
          t->session().find_task((pid_t)t->regs().arg2_signed());
      int cpu = t->regs().arg3_signed();
      int group_fd = t->regs().arg4_signed();
      unsigned long flags = t->regs().arg5();
      if (target && cpu == -1 && group_fd == -1 && !flags) {
        auto attr =
            t->read_mem(remote_ptr<struct perf_event_attr>(t->regs().arg1()));
        if (VirtualPerfCounterMonitor::should_virtualize(attr)) {
          Registers r = t->regs();
          // Turn this into a socket() syscall. This just gives us an allocated
          // fd. Syscalls using this fd will be emulated (except for close()).
          r.set_original_syscallno(Arch::socket);
          r.set_arg1(AF_UNIX);
          r.set_arg2(SOCK_STREAM);
          r.set_arg3(0);
          t->set_regs(r);
        }
      }
      return PREVENT_SWITCH;
    }

    case Arch::open: {
      string pathname = t->read_c_str(remote_ptr<char>(t->regs().arg1()));
      if (is_blacklisted_filename(pathname.c_str())) {
        LOG(warn) << "Cowardly refusing to open " << pathname;
        Registers r = t->regs();
        // Set path to terminating null byte. This forces ENOENT.
        r.set_arg1(remote_ptr<char>(r.arg1()) + pathname.size());
        t->set_regs(r);
      }
      return PREVENT_SWITCH;
    }

    case Arch::close:
      if (!t->fd_table()->allow_close((int)t->regs().arg1())) {
        // Don't let processes close this fd. Abort with EBADF by setting
        // oldfd to -1, as if the fd is already closed.
        Registers r = t->regs();
        r.set_arg1(intptr_t(-1));
        t->set_regs(r);
      }
      return PREVENT_SWITCH;

    case Arch::dup2:
    case Arch::dup3:
      if (!t->fd_table()->allow_close((int)t->regs().arg2())) {
        // Don't let processes dup over this fd. Abort with EBADF by setting
        // oldfd to -1.
        Registers r = t->regs();
        r.set_arg1(intptr_t(-1));
        t->set_regs(r);
      }
      return PREVENT_SWITCH;

    /* int prctl(int option, unsigned long arg2, unsigned long arg3, unsigned
     * long arg4, unsigned long arg5); */
    case Arch::prctl:
      switch ((int)t->regs().arg1_signed()) {
        case PR_GET_ENDIAN:
        case PR_GET_FPEMU:
        case PR_GET_FPEXC:
        case PR_GET_PDEATHSIG:
        case PR_GET_UNALIGN:
          syscall_state.reg_parameter<int>(2);
          break;

        case PR_GET_KEEPCAPS:
        case PR_GET_NO_NEW_PRIVS:
        case PR_GET_TIMERSLACK:
        case PR_MCE_KILL:
        case PR_MCE_KILL_GET:
        case PR_SET_KEEPCAPS:
        case PR_SET_NAME:
        case PR_SET_PDEATHSIG:
        case PR_SET_TIMERSLACK:
          break;

        case PR_SET_DUMPABLE:
          if (t->regs().arg2() == 0) {
            // Don't let processes make themslves undumpable. If a process
            // becomes undumpable, calling perf_event_open on it fails.
            Registers r = t->regs();
            r.set_arg1(intptr_t(-1));
            t->set_regs(r);
            syscall_state.emulate_result(0);
            t->task_group()->dumpable = false;
          } else if (t->regs().arg2() == 1) {
            t->task_group()->dumpable = true;
          }
          break;

        case PR_GET_DUMPABLE:
          syscall_state.emulate_result(t->task_group()->dumpable);
          break;

        case PR_GET_SECCOMP:
          syscall_state.emulate_result(t->prctl_seccomp_status);
          break;

        case PR_GET_TSC: {
          // Prevent the actual GET_TSC call and return our emulated state.
          Registers r = t->regs();
          r.set_arg1(intptr_t(-1));
          t->set_regs(r);
          syscall_state.emulate_result(0);
          t->write_mem(syscall_state.reg_parameter<int>(2, IN_OUT_NO_SCRATCH),
                       t->tsc_mode);
          break;
        }

        case PR_SET_TSC: {
          // Prevent the actual SET_TSC call.
          Registers r = t->regs();
          r.set_arg1(intptr_t(-1));
          t->set_regs(r);
          int val = (int)t->regs().arg2();
          if (val != PR_TSC_ENABLE && val != PR_TSC_SIGSEGV) {
            syscall_state.emulate_result(-EINVAL);
          } else {
            syscall_state.emulate_result(0);
            t->tsc_mode = val;
          }
          break;
        }

        case PR_GET_NAME:
          syscall_state.reg_parameter(2, 16);
          break;

        case PR_SET_NO_NEW_PRIVS:
          if ((unsigned long)t->regs().arg2() != 1) {
            syscall_state.expect_errno = EINVAL;
          }
          break;

        case PR_SET_SECCOMP:
          // Allow all known seccomp calls. We must allow the seccomp call
          // that rr triggers when spawning the initial tracee.
          switch ((unsigned long)t->regs().arg2()) {
            case SECCOMP_MODE_STRICT:
              break;
            case SECCOMP_MODE_FILTER: {
              // If we're bootstrapping then this must be rr's own syscall
              // filter, so just install it normally now.
              if (t->session().done_initial_exec()) {
                // Prevent the actual prctl call. We'll fix this up afterwards.
                Registers r = t->regs();
                r.set_arg1(intptr_t(-1));
                t->set_regs(r);
              }
              break;
            }
            default:
              syscall_state.expect_errno = EINVAL;
              break;
          }
          break;

        case PR_SET_PTRACER: {
          // Prevent any PR_SET_PTRACER call, but pretend it succeeded, since
          // we don't want any interference with our ptracing.
          Registers r = t->regs();
          r.set_arg1(intptr_t(-1));
          t->set_regs(r);
          syscall_state.emulate_result(0);
          break;
        }

        default:
          syscall_state.expect_errno = EINVAL;
          break;
      }
      return PREVENT_SWITCH;

    case Arch::arch_prctl:
      switch ((int)t->regs().arg1_signed()) {
        case ARCH_SET_FS:
        case ARCH_SET_GS:
          break;

        case ARCH_GET_FS:
        case ARCH_GET_GS:
          syscall_state.reg_parameter<typename Arch::unsigned_long>(2);
          break;

        default:
          syscall_state.expect_errno = EINVAL;
          break;
      }
      return PREVENT_SWITCH;

    case Arch::ioctl:
      return prepare_ioctl<Arch>(t, syscall_state);

    case Arch::_sysctl: {
      auto argsp =
          syscall_state.reg_parameter<typename Arch::__sysctl_args>(1, IN);
      auto oldlenp = syscall_state.mem_ptr_parameter_inferred(
          REMOTE_PTR_FIELD(argsp, oldlenp), IN_OUT);
      syscall_state.mem_ptr_parameter(
          REMOTE_PTR_FIELD(argsp, oldval),
          ParamSize::from_initialized_mem(t, oldlenp));
      return PREVENT_SWITCH;
    }

    case Arch::quotactl:
      switch (t->regs().arg1() >> SUBCMDSHIFT) {
        case Q_GETQUOTA:
          syscall_state.reg_parameter<typename Arch::dqblk>(4);
          break;
        case Q_GETINFO:
          syscall_state.reg_parameter<typename Arch::dqinfo>(4);
          break;
        case Q_GETFMT:
          syscall_state.reg_parameter<int>(4);
          break;
        case Q_SETQUOTA:
          FATAL() << "Trying to set disk quota usage, this may interfere with "
                     "rr recording";
        // not reached
        case Q_QUOTAON:
        case Q_QUOTAOFF:
        case Q_SETINFO:
        case Q_SYNC:
          break;
        default:
          syscall_state.expect_errno = EINVAL;
          break;
      }
      return PREVENT_SWITCH;

    /* int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int
     * timeout); */
    case Arch::epoll_wait:
      syscall_state.reg_parameter(2, sizeof(typename Arch::epoll_event) *
                                         t->regs().arg3_signed());
      return ALLOW_SWITCH;

    /* The following two syscalls enable context switching not for
     * liveness/correctness reasons, but rather because if we
     * didn't context-switch away, rr might end up busy-waiting
     * needlessly.  In addition, albeit far less likely, the
     * client program may have carefully optimized its own context
     * switching and we should take the hint. */

    case Arch::nanosleep:
      syscall_state.reg_parameter<typename Arch::timespec>(2);
      return ALLOW_SWITCH;

    case Arch::clock_nanosleep:
      syscall_state.reg_parameter<typename Arch::timespec>(4);
      return ALLOW_SWITCH;

    case Arch::sched_yield:
      t->session().scheduler().schedule_one_round_robin(t);
      return ALLOW_SWITCH;

    case Arch::rt_sigpending:
      syscall_state.reg_parameter(1, (size_t)t->regs().arg2());
      return PREVENT_SWITCH;

    case Arch::rt_sigtimedwait:
      syscall_state.reg_parameter<typename Arch::siginfo_t>(2);
      return ALLOW_SWITCH;

    case Arch::rt_sigsuspend:
    case Arch::sigsuspend:
      t->sigsuspend_blocked_sigs = unique_ptr<sig_set_t>(
          new sig_set_t(t->read_mem(remote_ptr<sig_set_t>(t->regs().arg1()))));
      return ALLOW_SWITCH;

    case Arch::rt_sigprocmask:
    case Arch::sigprocmask: {
      syscall_state.reg_parameter<typename Arch::sigset_t>(3);
      remote_ptr<sig_set_t> setp = t->regs().arg2();
      if (!setp.is_null()) {
        auto sig_set = t->read_mem(setp);
        syscall_state.saved_data.resize(sizeof(sig_set));
        memcpy(syscall_state.saved_data.data(), &sig_set, sizeof(sig_set));
        // Don't let the tracee block TIME_SLICE_SIGNAL or
        // SYSCALLBUF_DESCHED_SIGNAL.
        sig_set &= ~(uint64_t(1) << (PerfCounters::TIME_SLICE_SIGNAL - 1)) &
                   ~(uint64_t(1) << (SYSCALLBUF_DESCHED_SIGNAL - 1));
        t->write_mem(setp, sig_set);
      }
      return PREVENT_SWITCH;
    }

    case Arch::getxattr:
    case Arch::lgetxattr:
    case Arch::fgetxattr:
      syscall_state.reg_parameter(
          3, ParamSize::from_syscall_result<ssize_t>(t->regs().arg4()));
      return PREVENT_SWITCH;

    case Arch::listxattr:
    case Arch::llistxattr:
    case Arch::flistxattr:
      syscall_state.reg_parameter(
          2, ParamSize::from_syscall_result<ssize_t>(t->regs().arg3()));
      return PREVENT_SWITCH;

    case Arch::sched_setaffinity: {
      // Ignore all sched_setaffinity syscalls. They might interfere
      // with our own affinity settings.
      Registers r = t->regs();
      // Set arg1 to an invalid PID to ensure this syscall is ignored.
      r.set_arg1(-1);
      t->set_regs(r);
      syscall_state.emulate_result(0);
      return PREVENT_SWITCH;
    }

    case Arch::sched_getaffinity:
      syscall_state.reg_parameter(3, ParamSize::from_syscall_result<int>(
                                         (unsigned int)t->regs().arg2()));
      return PREVENT_SWITCH;

    case Arch::ptrace:
      return prepare_ptrace<Arch>(t, syscall_state);

    case Arch::mincore:
      syscall_state.reg_parameter(3, (t->regs().arg2() + page_size() - 1) /
                                         page_size());
      return PREVENT_SWITCH;

    case Arch::shmctl:
      return prepare_shmctl<Arch>(syscall_state, (int)t->regs().arg2_signed(),
                                  3);

    case Arch::semctl:
      return prepare_semctl<Arch>(
          t, syscall_state, (int)t->regs().arg1_signed(),
          (int)t->regs().arg3_signed(), 4, USE_DIRECTLY);

    case Arch::seccomp:
      switch ((unsigned int)t->regs().arg1()) {
        case SECCOMP_SET_MODE_STRICT:
          break;
        case SECCOMP_SET_MODE_FILTER: {
          // Prevent the actual seccomp call. We'll fix this up afterwards.
          Registers r = t->regs();
          r.set_arg1(intptr_t(-1));
          t->set_regs(r);
          break;
        }
        default:
          syscall_state.expect_errno = EINVAL;
          break;
      }
      return PREVENT_SWITCH;

    case Arch::madvise:
      switch ((int)t->regs().arg3()) {
        case MADV_NORMAL:
        case MADV_RANDOM:
        case MADV_SEQUENTIAL:
        case MADV_WILLNEED:
        case MADV_DONTNEED:
        case MADV_REMOVE:
        case MADV_DONTFORK:
        case MADV_DOFORK:
        case MADV_SOFT_OFFLINE:
        case MADV_HWPOISON:
        case MADV_MERGEABLE:
        case MADV_UNMERGEABLE:
        case MADV_HUGEPAGE:
        case MADV_NOHUGEPAGE:
        case MADV_DONTDUMP:
        case MADV_DODUMP:
          break;
        default:
          syscall_state.expect_errno = EINVAL;
      }
      return PREVENT_SWITCH;

    case Arch::personality:
      switch ((int)t->regs().arg1()) {
        case PER_LINUX:
          // The default personality requires no handling.
          break;
        case -1:
          // A special argument that only returns the existing personality.
          break;
        default:
          syscall_state.expect_errno = EINVAL;
      }
      return PREVENT_SWITCH;

    case Arch::mmap:
      switch (Arch::mmap_semantics) {
        case Arch::StructArguments: {
          auto args = t->read_mem(
              remote_ptr<typename Arch::mmap_args>(t->regs().arg1()));
          // XXX fix this
          ASSERT(t, !(args.flags & MAP_GROWSDOWN));
          break;
        }
        case Arch::RegisterArguments: {
          prepare_mmap_register_params(t);
          break;
        }
      }
      return PREVENT_SWITCH;

    case Arch::mmap2:
      prepare_mmap_register_params(t);
      return PREVENT_SWITCH;

    case Arch::mprotect:
      // Since we're stripping MAP_GROWSDOWN from kernel mmap calls, we need
      // to implement PROT_GROWSDOWN ourselves.
      t->vm()->fixup_mprotect_growsdown_parameters(t);
      return PREVENT_SWITCH;

    case SYS_rrcall_notify_control_msg:
    case SYS_rrcall_init_preload:
      syscall_state.emulate_result(0);
      return PREVENT_SWITCH;

    case SYS_rrcall_init_buffers:
      syscall_state.reg_parameter<rrcall_init_buffers_params<Arch> >(1, IN_OUT);
      return PREVENT_SWITCH;

    case Arch::brk:
    case Arch::munmap:
    case Arch::process_vm_readv:
    case Arch::process_vm_writev:
    case SYS_rrcall_notify_syscall_hook_exit:
    case Arch::shmat:
    case Arch::shmdt:
      return PREVENT_SWITCH;

    default:
      // Invalid syscalls return -ENOSYS. Assume any such
      // result means the syscall was completely ignored by the
      // kernel so it's OK for us to not do anything special.
      // Other results mean we probably need to understand this
      // syscall, but we don't.
      syscall_state.expect_errno = ENOSYS;
      return PREVENT_SWITCH;
  }
}

static Switchable rec_prepare_syscall_internal(
    RecordTask* t, TaskSyscallState& syscall_state) {
  RR_ARCH_FUNCTION(rec_prepare_syscall_arch, t->arch(), t, syscall_state)
}

Switchable rec_prepare_syscall(RecordTask* t) {
  auto& syscall_state = syscall_state_property.get_or_create(*t);
  syscall_state.init(t);

  Switchable s = rec_prepare_syscall_internal(t, syscall_state);
  int syscallno = t->ev().Syscall().number;
  if (is_sigreturn(syscallno, t->arch())) {
    // There isn't going to be an exit event for this syscall, so remove
    // syscall_state now.
    syscall_state_property.remove(*t);
    return s;
  }
  return syscall_state.done_preparing(s);
}

template <typename Arch>
static void rec_prepare_restart_syscall_arch(RecordTask* t,
                                             TaskSyscallState& syscall_state) {
  int syscallno = t->ev().Syscall().number;
  switch (syscallno) {
    case Arch::nanosleep:
    case Arch::clock_nanosleep:
      /* Hopefully uniquely among syscalls, nanosleep()/clock_nanosleep()
       * requires writing to its remaining-time outparam
       * *only if* the syscall fails with -EINTR.  When a
       * nanosleep() is interrupted by a signal, we don't
       * know a priori whether it's going to be eventually
       * restarted or not.  (Not easily, anyway.)  So we
       * don't know whether it will eventually return -EINTR
       * and would need the outparam written.  To resolve
       * that, we do what the kernel does, and update the
       * outparam at the -ERESTART_RESTART interruption
       * regardless. */
      syscall_state.process_syscall_results();
      break;
    case Arch::wait4:
    case Arch::waitid:
    case Arch::waitpid: {
      Registers r = t->regs();
      r.set_original_syscallno(
          syscall_state.syscall_entry_registers.original_syscallno());
      t->set_regs(r);
      t->in_wait_type = WAIT_TYPE_NONE;
      break;
    }
  }
}

static void rec_prepare_restart_syscall_internal(
    RecordTask* t, TaskSyscallState& syscall_state) {
  RR_ARCH_FUNCTION(rec_prepare_restart_syscall_arch, t->arch(), t,
                   syscall_state);
}

void rec_prepare_restart_syscall(RecordTask* t) {
  auto& syscall_state = *syscall_state_property.get(*t);
  rec_prepare_restart_syscall_internal(t, syscall_state);
  syscall_state_property.remove(*t);
}

static void process_execve(RecordTask* t, TaskSyscallState& syscall_state) {
  Registers r = t->regs();
  if (r.syscall_failed()) {
    return;
  }

  t->post_exec_syscall(*syscall_state.exec_saved_event);

  t->session().trace_writer().write_task_event(*syscall_state.exec_saved_event);

  KernelMapping vvar;

  // Write out stack mappings first since during replay we need to set up the
  // stack before any files get mapped.
  vector<KernelMapping> stacks;
  for (auto m : t->vm()->maps()) {
    auto& km = m.map;
    if (km.is_stack()) {
      stacks.push_back(km);
    } else if (km.is_vvar()) {
      vvar = km;
    }
  }

  {
    AutoRemoteSyscalls remote(t, AutoRemoteSyscalls::DISABLE_MEMORY_PARAMS);

    if (vvar.size()) {
      // We're not going to map [vvar] during replay --- that wouldn't
      // make sense, since it contains data from the kernel that isn't correct
      // for replay, and we patch out the vdso syscalls that would use it.
      // Unmapping it now makes recording look more like replay.
      // Also note that under 4.0.7-300.fc22.x86_64 (at least) /proc/<pid>/mem
      // can't read the contents of [vvar].
      remote.infallible_syscall(syscall_number_for_munmap(remote.arch()),
                                vvar.start(), vvar.size());
      t->vm()->unmap(vvar.start(), vvar.size());
    }

    for (auto& km : stacks) {
      auto mode = t->trace_writer().write_mapped_region(
          t, km, km.fake_stat(), TraceWriter::EXEC_MAPPING);
      ASSERT(t, mode == TraceWriter::RECORD_IN_TRACE);
      auto buf = t->read_mem(km.start().cast<uint8_t>(), km.size());
      t->trace_writer().write_raw(buf.data(), km.size(), km.start());

      // Remove MAP_GROWSDOWN from stacks by remapping the memory and
      // writing the contents back.
      int flags = (km.flags() & ~MAP_GROWSDOWN) | MAP_ANONYMOUS;
      remote.infallible_syscall(syscall_number_for_munmap(remote.arch()),
                                km.start(), km.size());
      if (!t->vm()->has_mapping(km.start() - page_size())) {
        // Unmap an extra page at the start; this seems to be necessary
        // to properly wipe out the growsdown mapping. Doing it as a separate
        // munmap call also seems to be necessary.
        remote.infallible_syscall(syscall_number_for_munmap(remote.arch()),
                                  km.start() - page_size(), page_size());
      }
      remote.infallible_mmap_syscall(km.start(), km.size(), km.prot(), flags,
                                     -1, 0);
      t->write_mem(km.start().cast<uint8_t>(), buf.data(), buf.size());
    }
  }

  // The kernel may zero part of the last page in each data mapping according
  // to ELF BSS metadata. So we record the last page of each data mapping in
  // the trace.
  vector<remote_ptr<void> > pages_to_record;

  for (auto m : t->vm()->maps()) {
    auto& km = m.map;
    if (km.start() == AddressSpace::rr_page_start()) {
      continue;
    }
    if (km.is_stack() || km.is_vsyscall()) {
      // [stack] has already been handled.
      // [vsyscall] can't be read via /proc/<pid>/mem, *should*
      // be the same across all execs, and can't be munmapped so we can't fix
      // it even if it does vary. Plus no-one should be using it anymore.
      continue;
    }
    struct stat st;
    if (stat(km.fsname().c_str(), &st) != 0) {
      st = km.fake_stat();
    }
    if (t->trace_writer().write_mapped_region(t, km, st,
                                              TraceWriter::EXEC_MAPPING) ==
        TraceWriter::RECORD_IN_TRACE) {
      if (st.st_size > 0) {
        off64_t end = (off64_t)st.st_size - km.file_offset_bytes();
        t->record_remote(km.start(), min(end, (off64_t)km.size()));
      } else {
        // st_size is not valid. Some device files are mmappable but have zero
        // size. We also take this path if there's no file at all (vdso etc).
        t->record_remote(km.start(), km.size());
      }
    } else {
      // See https://github.com/mozilla/rr/issues/1568; in some cases
      // after exec we have memory areas that are rwx. These areas have
      // a trailing page that may be partially zeroed by the kernel. Record the
      // trailing page of every mapping just to be simple and safe.
      pages_to_record.push_back(km.end() - page_size());
    }
  }

  init_scratch_memory(t, FIXED_ADDRESS);

  for (auto& p : pages_to_record) {
    t->record_remote(p, page_size());
  }

  // Patch LD_PRELOAD and VDSO after saving the mappings. Replay will apply
  // patches to the saved mappings.
  t->vm()->monkeypatcher().patch_after_exec(t);
}

static void process_mmap(RecordTask* t, size_t length, int prot, int flags,
                         int fd, off_t offset_pages) {
  size_t size = ceil_page_size(length);
  off64_t offset = offset_pages * 4096;

  if (t->regs().syscall_failed()) {
    // We purely emulate failed mmaps.
    return;
  }
  remote_ptr<void> addr = t->regs().syscall_result();
  if (flags & MAP_ANONYMOUS) {
    if (flags & MAP_PRIVATE) {
      // Anonymous mappings are by definition not backed by any file-like
      // object, and are initialized to zero, so there's no nondeterminism to
      // record.
      t->vm()->map(addr, size, prot, flags, 0, string(),
                   KernelMapping::NO_DEVICE, KernelMapping::NO_INODE);
    } else {
      ASSERT(t, !(flags & MAP_GROWSDOWN));
      // Read the kernel's mapping. There doesn't seem to be any other way to
      // get the correct device/inode numbers. Fortunately anonymous shared
      // mappings are rare.
      KernelMapping kernel_info = t->vm()->read_kernel_mapping(t, addr);
      KernelMapping km =
          t->vm()->map(addr, size, prot, flags, 0, kernel_info.fsname(),
                       kernel_info.device(), kernel_info.inode());
      auto d = t->trace_writer().write_mapped_region(t, km, km.fake_stat());
      ASSERT(t, d == TraceWriter::DONT_RECORD_IN_TRACE);
    }
    return;
  }

  ASSERT(t, fd >= 0) << "Valid fd required for file mapping";
  ASSERT(t, !(flags & MAP_GROWSDOWN));

  // TODO: save a reflink copy of the resource to the
  // trace directory as |fs/[st_dev].[st_inode]|.  Then
  // we wouldn't have to care about looking up a name
  // for the resource.
  auto result = t->stat_fd(fd);
  string file_name = t->file_name_of_fd(fd);

  KernelMapping km = t->vm()->map(addr, size, prot, flags, offset, file_name,
                                  result.st_dev, result.st_ino);

  if (t->trace_writer().write_mapped_region(t, km, result) ==
      TraceWriter::RECORD_IN_TRACE) {
    if (result.st_size > 0) {
      off64_t end = (off64_t)result.st_size - offset;
      t->record_remote(addr, min(end, (off64_t)size));
    } else {
      // st_size is not valid. Some device files are mmappable but have zero
      // size.
      t->record_remote(addr, size);
    }
  }

  if ((prot & PROT_WRITE) && (flags & MAP_SHARED)) {
    LOG(debug) << file_name << " is SHARED|WRITEABLE; that's not handled "
                               "correctly yet. Optimistically hoping it's not "
                               "written by programs outside the rr tracee "
                               "tree.";
  }

  t->vm()->monkeypatcher().patch_after_mmap(t, addr, size, offset_pages, fd);
}

static void process_shmat(RecordTask* t, int shmid, int shm_flags,
                          remote_ptr<void> addr) {
  if (t->regs().syscall_failed()) {
    // We purely emulate failed shmats.
    return;
  }

  struct shmid64_ds ds;
  int ret = _shmctl(shmid, IPC_STAT, &ds);
  ASSERT(t, !ret) << "shmid should be readable by rr since rr has the same "
                     "UID as tracees";
  size_t size = ceil_page_size(ds.shm_segsz);

  int prot = shm_flags_to_mmap_prot(shm_flags);
  int flags = MAP_SHARED;

  // Read the kernel's mapping for the shm segment. There doesn't seem to be
  // any other way to get the correct device number. (The inode number seems to
  // be the shm key.) This should be OK since SysV shmem is not used very much
  // and reading /proc/<pid>/maps should be reasonably cheap.
  KernelMapping kernel_info = t->vm()->read_kernel_mapping(t, addr);
  KernelMapping km =
      t->vm()->map(addr, size, prot, flags, 0, kernel_info.fsname(),
                   kernel_info.device(), kernel_info.inode());
  if (t->trace_writer().write_mapped_region(t, km, km.fake_stat()) ==
      TraceWriter::RECORD_IN_TRACE) {
    t->record_remote(addr, size);
  }

  LOG(debug) << "Optimistically hoping that SysV segment is not used outside "
                "of tracees";
}

template <typename Arch>
static string extra_expected_errno_info(RecordTask* t,
                                        TaskSyscallState& syscall_state) {
  stringstream ss;
  switch (syscall_state.expect_errno) {
    case ENOSYS:
      ss << "; execution of syscall unsupported by rr";
      break;
    case EINVAL:
      switch (t->regs().original_syscallno()) {
        case Arch::ioctl: {
          int request = (int)t->regs().arg2_signed();
          int type = _IOC_TYPE(request);
          int nr = _IOC_NR(request);
          int dir = _IOC_DIR(request);
          int size = _IOC_SIZE(request);
          ss << "; Unknown ioctl(" << HEX(request) << "): type:" << HEX(type)
             << " nr:" << HEX(nr) << " dir:" << HEX(dir) << " size:" << size
             << " addr:" << HEX(t->regs().arg3());
          break;
        }
        case Arch::quotactl:
          ss << "; unknown quotactl(" << HEX(t->regs().arg1() >> SUBCMDSHIFT)
             << ")";
          break;
        case Arch::fcntl:
        case Arch::fcntl64:
          ss << "; unknown fcntl(" << HEX((int)t->regs().arg2_signed()) << ")";
          break;
        case Arch::prctl:
          ss << "; unknown prctl(" << HEX((int)t->regs().arg1_signed()) << ")";
          break;
        case Arch::arch_prctl:
          ss << "; unknown arch_prctl(" << HEX((int)t->regs().arg1_signed())
             << ")";
          break;
        case Arch::socketcall:
          ss << "; unknown socketcall(" << HEX((int)t->regs().arg1_signed())
             << ")";
          break;
        case Arch::ipc:
          ss << "; unknown ipc(" << HEX((int)t->regs().arg1_signed()) << ")";
          break;
        case Arch::futex:
          ss << "; unknown futex("
             << HEX((int)t->regs().arg2_signed() & FUTEX_CMD_MASK) << ")";
          break;
        case Arch::waitid:
          ss << "; unknown waitid(" << HEX((idtype_t)t->regs().arg1()) << ")";
          break;
        case Arch::seccomp:
          ss << "; unknown seccomp(" << HEX((unsigned int)t->regs().arg1())
             << ")";
          break;
        case Arch::madvise:
          ss << "; unknown madvise(" << (int)t->regs().arg3() << ")";
          break;
      }
      break;
    case EIO:
      switch (t->regs().original_syscallno()) {
        case Arch::ptrace:
          ss << "; unsupported ptrace(" << HEX((int)t->regs().arg1()) << " ["
             << ptrace_req_name((int)t->regs().arg1_signed()) << "])";
          break;
      }
      break;
  }
  return ss.str();
}

static int dev_tty_fd() {
  static int fd = -1;
  if (fd < 0) {
    fd = open("/dev/tty", O_WRONLY);
  }
  return fd;
}

template <typename Arch>
static void record_iovec_output(RecordTask* t, RecordTask* dest,
                                remote_ptr<typename Arch::iovec> piov,
                                uint32_t iov_cnt) {
  // Ignore the syscall result, the kernel may have written more data than that.
  // See https://bugzilla.kernel.org/show_bug.cgi?id=113541
  auto iovs = t->read_mem(piov, iov_cnt);
  for (auto& iov : iovs) {
    dest->record_remote_fallible(iov.iov_base, iov.iov_len);
  }
}

static void handle_opened_file(RecordTask* t, int fd) {
  string pathname = t->file_name_of_fd(fd);

  bool do_write = false;
  // This must be kept in sync with replay_syscall's handle_opened_files.
  if (is_dev_tty(pathname.c_str())) {
    // This will let rr event annotations echo to /dev/tty. It will also
    // ensure writes to this fd are not syscall-buffered.
    // XXX the tracee's /dev/tty could refer to a tty other than
    // the recording tty, in which case output should not be
    // redirected. That's not too bad, replay will still work, just
    // with some spurious echoes.
    t->fd_table()->add_monitor(fd, new StdioMonitor(dev_tty_fd()));
    do_write = true;
  } else if (is_proc_mem_file(pathname.c_str())) {
    t->fd_table()->add_monitor(fd, new ProcMemMonitor(t, pathname));
    do_write = true;
  }

  if (do_write) {
    // Write absolute file name
    t->trace_writer().write_generic(&fd, sizeof(fd));
    t->trace_writer().write_generic(pathname.c_str(), pathname.size());
  }
}

template <typename Arch>
static void check_scm_rights_fd(RecordTask* t, typename Arch::msghdr& msg) {
  if (msg.msg_controllen < sizeof(typename Arch::cmsghdr)) {
    return;
  }
  auto data = t->read_mem(msg.msg_control.rptr().template cast<uint8_t>(),
                          msg.msg_controllen);
  size_t index = 0;
  while (true) {
    auto cmsg = reinterpret_cast<typename Arch::cmsghdr*>(data.data() + index);
    if (cmsg->cmsg_len < sizeof(*cmsg) ||
        index + Arch::cmsg_align(cmsg->cmsg_len) > data.size()) {
      break;
    }
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
      int* fds = static_cast<int*>(Arch::cmsg_data(cmsg));
      int fd_count = (cmsg->cmsg_len - sizeof(*cmsg)) / sizeof(int);
      for (int i = 0; i < fd_count; ++i) {
        handle_opened_file(t, fds[i]);
      }
    }
    index += Arch::cmsg_align(cmsg->cmsg_len);
    if (index + sizeof(*cmsg) > data.size()) {
      break;
    }
  }
}

template <typename Arch>
static void rec_process_syscall_arch(RecordTask* t,
                                     TaskSyscallState& syscall_state) {
  int syscallno = t->ev().Syscall().number;

  LOG(debug) << t->tid << ": processing: " << t->ev()
             << " -- time: " << t->trace_time();

  if (const struct syscallbuf_record* rec = t->desched_rec()) {
    t->record_local(t->syscallbuf_child.cast<void>() +
                        (rec->extra_data - (uint8_t*)t->syscallbuf_hdr),
                    rec->size - sizeof(*rec), (uint8_t*)rec->extra_data);
    return;
  }

  if (syscall_state.expect_errno) {
    ASSERT(t, t->regs().syscall_result_signed() == -syscall_state.expect_errno)
        << "Expected " << errno_name(syscall_state.expect_errno) << " for '"
        << t->syscall_name(syscallno) << "' but got result "
        << t->regs().syscall_result_signed() << " (errno "
        << errno_name(-t->regs().syscall_result_signed()) << ")"
        << extra_expected_errno_info<Arch>(t, syscall_state);
    return;
  }

  // Here we handle syscalls that need work that can only happen after the
  // syscall completes --- and that our TaskSyscallState infrastructure can't
  // handle.
  switch (syscallno) {
    case Arch::fork:
    case Arch::vfork:
    case Arch::clone: {
      // On a 3.19.0-39-generic #44-Ubuntu kernel we have observed clone()
      // clearing the parity flag internally.
      Registers r = t->regs();
      r.set_flags(syscall_state.syscall_entry_registers.flags());
      t->set_regs(r);
      break;
    }

    case Arch::execve:
      process_execve(t, syscall_state);
      if (t->emulated_ptracer && !t->emulated_ptrace_seized &&
          !(t->emulated_ptrace_options & PTRACE_O_TRACEEXEC)) {
        // Inject legacy SIGTRAP-after-exec
        t->tgkill(SIGTRAP);
      }
      break;

    case Arch::brk: {
      remote_ptr<void> old_brk = ceil_page_size(t->vm()->current_brk());
      remote_ptr<void> new_brk = ceil_page_size(t->regs().syscall_result());
      KernelMapping km;
      if (old_brk < new_brk) {
        // Read the kernel's mapping. There doesn't seem to be any other way to
        // get the correct prot bits for heaps. Usually it's READ|WRITE but
        // there seem to be exceptions depending on system settings.
        KernelMapping kernel_info = t->vm()->read_kernel_mapping(t, old_brk);
        ASSERT(t, km.device() == KernelMapping::NO_DEVICE);
        ASSERT(t, km.inode() == KernelMapping::NO_INODE);
        km = kernel_info.subrange(old_brk, new_brk);
      } else {
        // Write a dummy KernelMapping that indicates an unmap
        km = KernelMapping(new_brk, old_brk, string(), KernelMapping::NO_DEVICE,
                           KernelMapping::NO_INODE, 0, 0, 0);
      }
      auto d = t->trace_writer().write_mapped_region(t, km, km.fake_stat());
      ASSERT(t, d == TraceWriter::DONT_RECORD_IN_TRACE);
      t->vm()->brk(t->regs().syscall_result(), km.prot());
      break;
    }

    case Arch::mmap:
      switch (Arch::mmap_semantics) {
        case Arch::StructArguments: {
          auto args = t->read_mem(
              remote_ptr<typename Arch::mmap_args>(t->regs().arg1()));
          process_mmap(t, args.len, args.prot, args.flags, args.fd,
                       args.offset / 4096);
          break;
        }
        case Arch::RegisterArguments: {
          Registers r = t->regs();
          r.set_arg1(syscall_state.syscall_entry_registers.arg1());
          r.set_arg4(syscall_state.syscall_entry_registers.arg4_signed());
          t->set_regs(r);
          process_mmap(t, (size_t)r.arg2(), (int)r.arg3_signed(),
                       (int)r.arg4_signed(), (int)r.arg5_signed(),
                       ((off_t)r.arg6_signed()) / 4096);
          break;
        }
      }
      break;

    case Arch::mmap2: {
      Registers r = t->regs();
      r.set_arg1(syscall_state.syscall_entry_registers.arg1());
      r.set_arg4(syscall_state.syscall_entry_registers.arg4_signed());
      t->set_regs(r);
      process_mmap(t, (size_t)r.arg2(), (int)r.arg3_signed(),
                   (int)r.arg4_signed(), (int)r.arg5_signed(),
                   (off_t)r.arg6_signed());
      break;
    }

    case Arch::shmat:
      process_shmat(t, (int)t->regs().arg1_signed(),
                    (int)t->regs().arg3_signed(), t->regs().syscall_result());
      break;

    case Arch::ipc:
      switch ((int)t->regs().arg1_signed()) {
        case SHMAT: {
          auto out_ptr = t->read_mem(
              remote_ptr<typename Arch::unsigned_long>(t->regs().arg4()));
          process_shmat(t, (int)t->regs().arg2_signed(),
                        (int)t->regs().arg3_signed(), out_ptr);
          break;
        }
        default:
          break;
      }
      break;

    case Arch::clock_nanosleep:
    case Arch::nanosleep: {
      /* If the sleep completes, the kernel doesn't
       * write back to the remaining-time
       * argument. */
      if (!(int)t->regs().syscall_result_signed()) {
        syscall_state.write_back = TaskSyscallState::NO_WRITE_BACK;
      }
      break;
    }

    case Arch::rt_sigsuspend:
    case Arch::sigsuspend:
      t->sigsuspend_blocked_sigs = nullptr;
      break;

    case Arch::rt_sigprocmask:
    case Arch::sigprocmask: {
      remote_ptr<sig_set_t> setp = t->regs().arg2();
      if (!setp.is_null()) {
        // Restore modified sig_set
        t->write_bytes_helper(setp, syscall_state.saved_data.size(),
                              syscall_state.saved_data.data());
      }
      break;
    }

    case Arch::perf_event_open:
      if (t->regs().original_syscallno() == Arch::socket) {
        ASSERT(t, !t->regs().syscall_failed());
        int fd = t->regs().syscall_result_signed();
        Registers r = t->regs();
        r.set_original_syscallno(
            syscall_state.syscall_entry_registers.original_syscallno());
        r.set_arg1(syscall_state.syscall_entry_registers.arg1());
        r.set_arg2(syscall_state.syscall_entry_registers.arg2());
        r.set_arg3(syscall_state.syscall_entry_registers.arg3());
        t->set_regs(r);
        auto attr =
            t->read_mem(remote_ptr<struct perf_event_attr>(t->regs().arg1()));
        t->fd_table()->add_monitor(
            fd, new VirtualPerfCounterMonitor(
                    t, t->session().find_task((pid_t)t->regs().arg2_signed()),
                    attr));
      }
      break;

    case Arch::open: {
      // Restore the registers that we may have altered.
      Registers r = t->regs();
      r.set_arg1(syscall_state.syscall_entry_registers.arg1());
      t->set_regs(r);
      if (!t->regs().syscall_failed()) {
        handle_opened_file(t, (int)t->regs().syscall_result_signed());
      }
      break;
    }

    case Arch::openat:
      if (!t->regs().syscall_failed()) {
        handle_opened_file(t, (int)t->regs().syscall_result_signed());
      }
      break;

    case SYS_rrcall_notify_control_msg: {
      auto msg =
          t->read_mem(remote_ptr<typename Arch::msghdr>(t->regs().arg1()));
      check_scm_rights_fd<Arch>(t, msg);
      break;
    }

    case Arch::recvmsg:
      if (!t->regs().syscall_failed()) {
        auto msg =
            t->read_mem(remote_ptr<typename Arch::msghdr>(t->regs().arg2()));
        check_scm_rights_fd<Arch>(t, msg);
      }
      break;

    case Arch::recvmmsg:
      if (!t->regs().syscall_failed()) {
        int msg_count = (int)t->regs().syscall_result_signed();
        auto msgs = t->read_mem(
            remote_ptr<typename Arch::mmsghdr>(t->regs().arg2()), msg_count);
        for (auto& m : msgs) {
          check_scm_rights_fd<Arch>(t, m.msg_hdr);
        }
      }
      break;

    case Arch::socketcall:
      if (!t->regs().syscall_failed()) {
        switch ((int)t->regs().arg1_signed()) {
          case SYS_RECVMSG: {
            auto args = t->read_mem(
                remote_ptr<typename Arch::recvmsg_args>(t->regs().arg2()));
            auto msg = t->read_mem(args.msg.rptr());
            check_scm_rights_fd<Arch>(t, msg);
            break;
          }
          case SYS_RECVMMSG: {
            auto args = t->read_mem(
                remote_ptr<typename Arch::recvmmsg_args>(t->regs().arg2()));
            int msg_count = (int)t->regs().syscall_result_signed();
            auto msgs = t->read_mem(args.msgvec.rptr(), msg_count);
            for (auto& m : msgs) {
              check_scm_rights_fd<Arch>(t, m.msg_hdr);
            }
            break;
          }
        }
      }
      break;

    case Arch::process_vm_readv:
      record_iovec_output<Arch>(t, t, t->regs().arg2(), t->regs().arg3());
      break;

    case Arch::process_vm_writev: {
      RecordTask* dest = t->session().find_task(t->regs().arg1());
      if (dest) {
        record_iovec_output<Arch>(t, dest, t->regs().arg4(), t->regs().arg5());
      }
      break;
    }

    case Arch::close:
    case Arch::dup2:
    case Arch::dup3:
    case Arch::fcntl:
    case Arch::fcntl64:
    case Arch::ioctl:
    case Arch::pread64:
    case Arch::preadv:
    case Arch::ptrace:
    case Arch::read:
    case Arch::readv:
    case Arch::sched_setaffinity:
    case Arch::mprotect: {
      // Restore the registers that we may have altered.
      Registers r = t->regs();
      r.set_arg1(syscall_state.syscall_entry_registers.arg1());
      r.set_arg2(syscall_state.syscall_entry_registers.arg2());
      r.set_arg3(syscall_state.syscall_entry_registers.arg3());
      t->set_regs(r);
      break;
    }

    case Arch::waitpid:
    case Arch::wait4:
    case Arch::waitid: {
      t->in_wait_type = WAIT_TYPE_NONE;
      // Restore possibly-modified registers
      Registers r = t->regs();
      r.set_arg1(syscall_state.syscall_entry_registers.arg1());
      r.set_arg2(syscall_state.syscall_entry_registers.arg2());
      r.set_arg3(syscall_state.syscall_entry_registers.arg3());
      r.set_arg4(syscall_state.syscall_entry_registers.arg4());
      r.set_original_syscallno(
          syscall_state.syscall_entry_registers.original_syscallno());
      t->set_regs(r);

      RecordTask* tracee = syscall_state.ptraced_tracee;
      if (tracee) {
        // Finish emulation of ptrace result
        Registers r = t->regs();
        r.set_syscall_result(tracee->tid);
        t->set_regs(r);
        if (syscallno == Arch::waitid) {
          remote_ptr<typename Arch::siginfo_t> sip = r.arg3();
          if (!sip.is_null()) {
            typename Arch::siginfo_t si;
            memset(&si, 0, sizeof(si));
            si.si_signo = SIGCHLD;
            si.si_code = CLD_TRAPPED;
            si._sifields._sigchld.si_pid_ = tracee->tgid();
            si._sifields._sigchld.si_uid_ = tracee->getuid();
            si._sifields._sigchld.si_status_ =
                tracee->emulated_ptrace_stop_code.ptrace_signal();
            t->write_mem(sip, si);
          }
        } else {
          remote_ptr<int> statusp = r.arg2();
          if (!statusp.is_null()) {
            t->write_mem(statusp, tracee->emulated_ptrace_stop_code.get());
          }
        }
        if (syscallno == Arch::waitid && (r.arg4() & WNOWAIT)) {
          // Leave the child in a waitable state
        } else {
          if (tracee->emulated_ptrace_stop_code.exit_code() >= 0) {
            // If we stopped the tracee to deliver this notification,
            // now allow it to continue to exit properly and notify its
            // real parent.
            ASSERT(t, tracee->ev().is_syscall_event() &&
                          PROCESSING_SYSCALL == tracee->ev().Syscall().state &&
                          tracee->stable_exit);
            // Continue the task since we didn't in enter_syscall
            tracee->resume_execution(RESUME_SYSCALL, RESUME_NONBLOCKING,
                                     RESUME_NO_TICKS);
          }
          tracee->emulated_ptrace_stop_pending = false;
        }
      }
      break;
    }

    case Arch::prctl: {
      // Restore arg1 in case we modified it to disable the syscall
      Registers r = t->regs();
      r.set_arg1(syscall_state.syscall_entry_registers.arg1());
      t->set_regs(r);
      switch ((int)t->regs().arg1()) {
        case PR_SET_SECCOMP:
          if (t->session().done_initial_exec()) {
            t->session()
                .as_record()
                ->seccomp_filter_rewriter()
                .install_patched_seccomp_filter(t);
            // install_patched_seccomp_filter can set registers to indicate
            // failure.
            if (!t->regs().syscall_failed()) {
              t->prctl_seccomp_status = 2;
            }
          }
          break;
      }
      break;
    }

    case Arch::seccomp: {
      // Restore arg1 in case we modified it to disable the syscall
      Registers r = t->regs();
      r.set_arg1(syscall_state.syscall_entry_registers.arg1());
      t->set_regs(r);
      if (t->regs().arg1() == SECCOMP_SET_MODE_FILTER) {
        t->session()
            .as_record()
            ->seccomp_filter_rewriter()
            .install_patched_seccomp_filter(t);
        // install_patched_seccomp_filter can set registers to indicate
        // failure.
        ASSERT(t, t->session().done_initial_exec())
            << "no seccomp calls during spawn";
        if (!t->regs().syscall_failed()) {
          t->prctl_seccomp_status = 2;
        }
      }
      break;
    }

    case SYS_rrcall_init_buffers:
      t->init_buffers();
      break;

    case SYS_rrcall_init_preload: {
      t->at_preload_init();
      break;
    }

    case SYS_rrcall_notify_syscall_hook_exit: {
      t->syscallbuf_hdr->notify_on_syscall_hook_exit = false;
      t->record_local(
          REMOTE_PTR_FIELD(t->syscallbuf_child, notify_on_syscall_hook_exit),
          &t->syscallbuf_hdr->notify_on_syscall_hook_exit);

      struct rrcall_params {
        typename Arch::unsigned_word result;
        typename Arch::unsigned_word original_syscallno;
      };
      Registers r = t->regs();
      auto params_ptr = r.sp() + sizeof(typename Arch::unsigned_word);
      auto params = t->read_mem(params_ptr.cast<rrcall_params>());
      r.set_syscall_result((uintptr_t)params.result);
      r.set_original_syscallno((intptr_t)params.original_syscallno);
      t->set_regs(r);
      break;
    }
  }
}

static void rec_process_syscall_internal(RecordTask* t,
                                         TaskSyscallState& syscall_state) {
  RR_ARCH_FUNCTION(rec_process_syscall_arch, t->arch(), t, syscall_state)
}

void rec_process_syscall(RecordTask* t) {
  auto& syscall_state = *syscall_state_property.get(*t);
  rec_process_syscall_internal(t, syscall_state);
  syscall_state.process_syscall_results();
  t->on_syscall_exit(t->ev().Syscall().number, t->regs());
  syscall_state_property.remove(*t);
}

} // namespace rr
