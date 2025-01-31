/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#include "AddressSpace.h"

#include <limits.h>
#include <linux/kdev_t.h>
#include <sys/stat.h>
#include <unistd.h>

#include <limits>

#include "rr/rr.h"

#include "preload/preload_interface.h"

#include "AutoRemoteSyscalls.h"
#include "log.h"
#include "RecordSession.h"
#include "RecordTask.h"
#include "Session.h"
#include "Task.h"

using namespace std;

namespace rr {

/*static*/ const uint8_t AddressSpace::breakpoint_insn;

/**
 * Advance *str to skip leading blank characters.
 */
static const char* trim_leading_blanks(const char* str) {
  const char* trimmed = str;
  while (isblank(*trimmed)) {
    ++trimmed;
  }
  return trimmed;
}

/**
 * The following helper is used to iterate over a tracee's memory
 * map.
 */
class KernelMapIterator {
public:
  KernelMapIterator(Task* t) : t(t) {
    char maps_path[PATH_MAX];
    sprintf(maps_path, "/proc/%d/maps", t->tid);
    ASSERT(t, (maps_file = fopen(maps_path, "r"))) << "Failed to open "
                                                   << maps_path;
    ++*this;
  }
  ~KernelMapIterator() {
    if (maps_file) {
      fclose(maps_file);
    }
  }
  // It's very important to keep in mind that btrfs files can have the wrong
  // device number!
  const KernelMapping& current(string* raw_line = nullptr) {
    if (raw_line) {
      *raw_line = this->raw_line;
    }
    return km;
  }
  bool at_end() { return !maps_file; }
  void operator++();

private:
  Task* t;
  FILE* maps_file;
  string raw_line;
  KernelMapping km;
};

void KernelMapIterator::operator++() {
  char line[PATH_MAX * 2];
  if (!fgets(line, sizeof(line), maps_file)) {
    fclose(maps_file);
    maps_file = nullptr;
    return;
  }

  uint64_t start, end, offset, inode;
  int dev_major, dev_minor;
  char flags[32];
  int chars_scanned;
  int nparsed = sscanf(line, "%" SCNx64 "-%" SCNx64 " %31s %" SCNx64
                             " %x:%x %" SCNu64 " %n",
                       &start, &end, flags, &offset, &dev_major, &dev_minor,
                       &inode, &chars_scanned);
  ASSERT(t, 8 /*number of info fields*/ == nparsed ||
                7 /*num fields if name is blank*/ == nparsed);

  // trim trailing newline, if any
  int last_char = strlen(line) - 1;
  if (line[last_char] == '\n') {
    line[last_char] = 0;
  }
  raw_line = line;

  const char* name = trim_leading_blanks(line + chars_scanned);
#if defined(__i386__)
  if (start > numeric_limits<uint32_t>::max() ||
      end > numeric_limits<uint32_t>::max() ||
      strcmp(name, "[vsyscall]") == 0) {
    // We manually read the exe link here because
    // this helper is used to set
    // |t->vm()->exe_image()|, so we can't rely on
    // that being correct yet.
    char proc_exe[PATH_MAX];
    char exe[PATH_MAX];
    snprintf(proc_exe, sizeof(proc_exe), "/proc/%d/exe", t->tid);
    readlink(proc_exe, exe, sizeof(exe));
    FATAL() << "Sorry, tracee " << t->tid << " has x86-64 image " << exe
            << " and that's not supported with a 32-bit rr.";
  }
#endif
  int prot = (strchr(flags, 'r') ? PROT_READ : 0) |
             (strchr(flags, 'w') ? PROT_WRITE : 0) |
             (strchr(flags, 'x') ? PROT_EXEC : 0);
  int f = (strchr(flags, 'p') ? MAP_PRIVATE : 0) |
          (strchr(flags, 's') ? MAP_SHARED : 0);

  km = KernelMapping(start, end, name, MKDEV(dev_major, dev_minor), inode, prot,
                     f, offset);
}

KernelMapping AddressSpace::read_kernel_mapping(Task* t,
                                                remote_ptr<void> addr) {
  MemoryRange range(addr, 1);
  for (KernelMapIterator it(t); !it.at_end(); ++it) {
    const KernelMapping& km = it.current();
    if (km.contains(range)) {
      return km;
    }
  }
  return KernelMapping();
}

/**
 * Cat the /proc/[t->tid]/maps file to stdout, line by line.
 */
static void print_process_mmap(Task* t) {
  for (KernelMapIterator it(t); !it.at_end(); ++it) {
    string line;
    it.current(&line);
    cerr << line << '\n';
  }
}

AddressSpace::~AddressSpace() { session_->on_destroy(this); }

void AddressSpace::after_clone() { allocate_watchpoints(); }

static remote_ptr<void> find_rr_vdso(Task* t, size_t* len) {
  for (KernelMapIterator it(t); !it.at_end(); ++it) {
    auto& km = it.current();
    if (km.fsname() == "[vdso]") {
      *len = km.size();
      ASSERT(t, uint32_t(*len) == *len) << "VDSO more than 4GB???";
      return km.start();
    }
  }
  ASSERT(t, false) << "rr VDSO not found?";
  return nullptr;
}

static uint32_t find_offset_of_syscall_instruction_in(SupportedArch arch,
                                                      uint8_t* vdso_data,
                                                      size_t vdso_len) {
  auto instruction = syscall_instruction(arch);
  for (uint32_t i = 1; i < vdso_len - instruction.size(); ++i) {
    if (memcmp(vdso_data + i, instruction.data(), instruction.size()) == 0) {
      return i;
    }
  }
  return 0;
}

uint32_t AddressSpace::offset_to_syscall_in_vdso[SupportedArch_MAX + 1];

remote_code_ptr AddressSpace::find_syscall_instruction(Task* t) {
  SupportedArch arch = t->arch();
  if (!offset_to_syscall_in_vdso[arch]) {
    auto vdso_data = t->read_mem(vdso().start().cast<uint8_t>(), vdso().size());
    offset_to_syscall_in_vdso[arch] = find_offset_of_syscall_instruction_in(
        arch, vdso_data.data(), vdso_data.size());
    ASSERT(t, offset_to_syscall_in_vdso[arch])
        << "No syscall instruction found in VDSO";
  }
  return remote_code_ptr(
      (vdso().start().cast<uint8_t>() + offset_to_syscall_in_vdso[arch])
          .as_int());
}

static string find_rr_page_file(Task* t) {
  string path = exe_directory() + "rr_page_";
  switch (t->arch()) {
    case x86:
      path += "32";
      break;
    case x86_64:
      path += "64";
      break;
    default:
      ASSERT(t, false) << "Unknown architecture";
      return path;
  }
  if (!t->session().is_recording()) {
    path += "_replay";
  }
  return path;
}

void AddressSpace::map_rr_page(Task* t) {
  int prot = PROT_EXEC | PROT_READ;
  int flags = MAP_PRIVATE | MAP_FIXED;

  struct stat fstat;
  string file_name;
  {
    AutoRemoteSyscalls remote(t);
    SupportedArch arch = t->arch();

    string path = find_rr_page_file(t);
    AutoRestoreMem child_path(remote, path.c_str());
    // skip leading '/' since we want the path to be relative to the root fd
    int child_fd = remote.infallible_syscall(syscall_number_for_openat(arch),
                                             RR_RESERVED_ROOT_DIR_FD,
                                             child_path.get() + 1, O_RDONLY);

    remote.infallible_mmap_syscall(rr_page_start(), rr_page_size(), prot, flags,
                                   child_fd, 0);

    fstat = t->stat_fd(child_fd);
    file_name = t->file_name_of_fd(child_fd);

    remote.infallible_syscall(syscall_number_for_close(arch), child_fd);

    if (t->session().is_recording()) {
      // brk() will not have been called yet so the brk area is empty.
      brk_start = brk_end =
          remote.infallible_syscall(syscall_number_for_brk(arch), 0);
    }
  }

  map(rr_page_start(), rr_page_size(), prot, flags, 0, file_name, fstat.st_dev,
      fstat.st_ino);

  traced_syscall_ip_ = rr_page_syscall_entry_point(
      TRACED, UNPRIVILEGED, RECORDING_AND_REPLAY, t->arch());
  privileged_traced_syscall_ip_ = rr_page_syscall_entry_point(
      TRACED, PRIVILEGED, RECORDING_AND_REPLAY, t->arch());
}

/**
 * Must match generate_rr_page.py
 */
static const AddressSpace::SyscallType entry_points[] = {
  { AddressSpace::TRACED, AddressSpace::UNPRIVILEGED,
    AddressSpace::RECORDING_AND_REPLAY },
  { AddressSpace::TRACED, AddressSpace::PRIVILEGED,
    AddressSpace::RECORDING_AND_REPLAY },
  { AddressSpace::UNTRACED, AddressSpace::UNPRIVILEGED,
    AddressSpace::RECORDING_AND_REPLAY },
  { AddressSpace::UNTRACED, AddressSpace::UNPRIVILEGED,
    AddressSpace::REPLAY_ONLY },
  { AddressSpace::UNTRACED, AddressSpace::UNPRIVILEGED,
    AddressSpace::RECORDING_ONLY },
  { AddressSpace::UNTRACED, AddressSpace::PRIVILEGED,
    AddressSpace::RECORDING_AND_REPLAY },
  { AddressSpace::UNTRACED, AddressSpace::PRIVILEGED,
    AddressSpace::REPLAY_ONLY },
  { AddressSpace::UNTRACED, AddressSpace::PRIVILEGED,
    AddressSpace::RECORDING_ONLY },
};

static remote_code_ptr ip_from_index(size_t i) {
  return remote_code_ptr(RR_PAGE_ADDR + RR_PAGE_SYSCALL_STUB_SIZE * i +
                         RR_PAGE_SYSCALL_INSTRUCTION_END);
}

remote_code_ptr AddressSpace::rr_page_syscall_exit_point(Traced traced,
                                                         Privileged privileged,
                                                         Enabled enabled) {
  for (auto& e : entry_points) {
    if (e.traced == traced && e.privileged == privileged &&
        e.enabled == enabled) {
      return ip_from_index(&e - entry_points);
    }
  }
  return nullptr;
}

const AddressSpace::SyscallType* AddressSpace::rr_page_syscall_from_exit_point(
    remote_code_ptr ip) {
  for (size_t i = 0; i < array_length(entry_points); ++i) {
    if (ip_from_index(i) == ip) {
      return &entry_points[i];
    }
  }
  return nullptr;
}

remote_code_ptr AddressSpace::rr_page_syscall_entry_point(Traced traced,
                                                          Privileged privileged,
                                                          Enabled enabled,
                                                          SupportedArch arch) {
  remote_code_ptr ip = rr_page_syscall_exit_point(traced, privileged, enabled);
  return ip.is_null() ? remote_code_ptr()
                      : ip.decrement_by_syscall_insn_length(arch);
}

vector<AddressSpace::SyscallType> AddressSpace::rr_page_syscalls() {
  vector<SyscallType> result;
  for (auto& e : entry_points) {
    result.push_back(e);
  }
  return result;
}

template <typename Arch> static vector<uint8_t> read_auxv_arch(Task* t) {
  auto stack_ptr = t->regs().sp().cast<typename Arch::unsigned_word>();

  auto argc = t->read_mem(stack_ptr);
  stack_ptr += argc + 1;

  // Check final NULL in argv
  auto null_ptr = t->read_mem(stack_ptr);
  assert(null_ptr == 0);
  stack_ptr++;

  // Should now point to envp
  while (0 != t->read_mem(stack_ptr)) {
    stack_ptr++;
  }
  stack_ptr++;
  // should now point to ELF Auxiliary Table

  vector<uint8_t> result;
  while (true) {
    auto pair_vec = t->read_mem(stack_ptr, 2);
    stack_ptr += 2;
    typename Arch::unsigned_word pair[2] = { pair_vec[0], pair_vec[1] };
    result.resize(result.size() + sizeof(pair));
    memcpy(result.data() + result.size() - sizeof(pair), pair, sizeof(pair));
    if (pair[0] == 0) {
      break;
    }
  }
  return result;
}

static vector<uint8_t> read_auxv(Task* t) {
  RR_ARCH_FUNCTION(read_auxv_arch, t->arch(), t);
}

void AddressSpace::save_auxv(Task* t) { saved_auxv_ = read_auxv(t); }

void AddressSpace::post_exec_syscall(Task* t) {
  // First locate a syscall instruction we can use for remote syscalls.
  traced_syscall_ip_ = find_syscall_instruction(t);
  privileged_traced_syscall_ip_ = nullptr;
  // Now remote syscalls work, we can open_mem_fd.
  t->open_mem_fd();
  // Now we can set up the "rr page" at its fixed address. This gives
  // us traced and untraced syscall instructions at known, fixed addresses.
  map_rr_page(t);
}

void AddressSpace::brk(remote_ptr<void> addr, int prot) {
  LOG(debug) << "brk(" << addr << ")";

  remote_ptr<void> old_brk = ceil_page_size(brk_end);
  remote_ptr<void> new_brk = ceil_page_size(addr);
  if (old_brk < new_brk) {
    map(old_brk, new_brk - old_brk, prot, MAP_ANONYMOUS | MAP_PRIVATE, 0,
        "[heap]", KernelMapping::NO_DEVICE, KernelMapping::NO_INODE);
  } else {
    unmap(new_brk, old_brk - new_brk);
  }
  brk_end = addr;
}

void AddressSpace::dump() const {
  fprintf(stderr, "  (heap: %p-%p)\n", (void*)brk_start.as_int(),
          (void*)brk_end.as_int());
  for (auto it = mem.begin(); it != mem.end(); ++it) {
    const KernelMapping& m = it->second.map;
    fprintf(stderr, "%s\n", m.str().c_str());
  }
}

SupportedArch AddressSpace::arch() const {
  return (*task_set().begin())->arch();
}

BreakpointType AddressSpace::get_breakpoint_type_for_retired_insn(
    remote_code_ptr ip) {
  remote_code_ptr addr = ip.decrement_by_bkpt_insn_length(SupportedArch::x86);
  return get_breakpoint_type_at_addr(addr);
}

BreakpointType AddressSpace::get_breakpoint_type_at_addr(remote_code_ptr addr) {
  auto it = breakpoints.find(addr);
  return it == breakpoints.end() ? BKPT_NONE : it->second.type();
}

bool AddressSpace::is_breakpoint_in_private_read_only_memory(
    remote_code_ptr addr) {
  for (const auto& m : maps_starting_at(addr.to_data_ptr<void>())) {
    if (m.map.start() >=
        addr.increment_by_bkpt_insn_length(arch()).to_data_ptr<void>()) {
      break;
    }
    if ((m.map.prot() & PROT_WRITE) || (m.map.flags() & MAP_SHARED)) {
      return false;
    }
  }
  return true;
}

void AddressSpace::replace_breakpoints_with_original_values(
    uint8_t* dest, size_t length, remote_ptr<uint8_t> addr) {
  for (auto& it : breakpoints) {
    remote_ptr<uint8_t> bkpt_location = it.first.to_data_ptr<uint8_t>();
    remote_ptr<uint8_t> start = max(addr, bkpt_location);
    remote_ptr<uint8_t> end =
        min(addr + length, bkpt_location + it.second.data_length());
    if (start < end) {
      memcpy(dest + (start - addr),
             it.second.original_data() + (start - bkpt_location), end - start);
    }
  }
}

bool AddressSpace::is_breakpoint_instruction(Task* t, remote_code_ptr ip) {
  bool ok = true;
  return t->read_mem(ip.to_data_ptr<uint8_t>(), &ok) == breakpoint_insn && ok;
}

static void remove_range(set<MemoryRange>& ranges, const MemoryRange& range) {
  auto start = ranges.lower_bound(range);
  auto end = start;
  auto prev_end = start;
  while (end != ranges.end() && end->start() < range.end()) {
    prev_end = end;
    ++end;
  }
  if (start == end) {
    return;
  }
  MemoryRange start_range = *start;
  MemoryRange end_range = *prev_end;
  ranges.erase(start, end);
  if (start_range.start() < range.start()) {
    ranges.insert(MemoryRange(start_range.start(), range.start()));
  }
  if (range.end() < end_range.end()) {
    ranges.insert(MemoryRange(range.end(), end_range.end()));
  }
}

static void add_range(set<MemoryRange>& ranges, const MemoryRange& range) {
  // Remove overlapping ranges
  remove_range(ranges, range);
  ranges.insert(range);
  // We could coalesce adjacent ranges, but there's probably no need.
}

KernelMapping AddressSpace::map(remote_ptr<void> addr, size_t num_bytes,
                                int prot, int flags, off64_t offset_bytes,
                                const string& fsname, dev_t device, ino_t inode,
                                const KernelMapping* recorded_map,
                                EmuFile::shr_ptr emu_file) {
  LOG(debug) << "mmap(" << addr << ", " << num_bytes << ", " << HEX(prot)
             << ", " << HEX(flags) << ", " << HEX(offset_bytes);
  num_bytes = ceil_page_size(num_bytes);
  KernelMapping m(addr, addr + num_bytes, fsname, device, inode, prot, flags,
                  offset_bytes);
  if (!num_bytes) {
    return m;
  }

  remove_range(dont_fork, MemoryRange(addr, num_bytes));

  // The mmap() man page doesn't specifically describe
  // what should happen if an existing map is
  // "overwritten" by a new map (of the same resource).
  // In testing, the behavior seems to be as if the
  // overlapping region is unmapped and then remapped
  // per the arguments to the second call.
  unmap_internal(addr, num_bytes);

  const KernelMapping& actual_recorded_map = recorded_map ? *recorded_map : m;
  map_and_coalesce(m, actual_recorded_map, emu_file);

  if ((prot & PROT_EXEC) &&
      (fsname.find(SYSCALLBUF_LIB_FILENAME) != string::npos ||
       fsname.find(SYSCALLBUF_LIB_FILENAME_32) != string::npos)) {
    syscallbuf_lib_start_ = addr;
    syscallbuf_lib_end_ = addr + num_bytes;
  }

  // During an emulated exec, we will explicitly map in a (copy of) the VDSO
  // at the recorded address.
  if (actual_recorded_map.is_vdso()) {
    vdso_start_addr = addr;
  }

  return m;
}

template <typename Arch> void AddressSpace::at_preload_init_arch(Task* t) {
  auto params = t->read_mem(
      remote_ptr<rrcall_init_preload_params<Arch> >(t->regs().arg1()));

  if (t->session().is_recording()) {
    ASSERT(t, t->session().as_record()->use_syscall_buffer() ==
                  params.syscallbuf_enabled)
        << "Tracee thinks syscallbuf is "
        << (params.syscallbuf_enabled ? "en" : "dis")
        << "abled, but tracer thinks "
        << (t->session().as_record()->use_syscall_buffer() ? "en" : "dis")
        << "abled";
  }

  if (!params.syscallbuf_enabled) {
    return;
  }

  syscallbuf_enabled_ = true;

  if (t->session().is_recording()) {
    monkeypatch_state->patch_at_preload_init(static_cast<RecordTask*>(t));
  }
}

void AddressSpace::at_preload_init(Task* t) {
  ASSERT(t, !syscallbuf_lib_start_.is_null())
      << "should have found preload library already";
  RR_ARCH_FUNCTION(at_preload_init_arch, t->arch(), t);
}

const AddressSpace::Mapping& AddressSpace::mapping_of(
    remote_ptr<void> addr) const {
  MemoryRange range(floor_page_size(addr), page_size());
  auto it = mem.find(range);
  assert(it != mem.end());
  assert(it->second.map.contains(range));
  return it->second;
}

bool AddressSpace::has_mapping(remote_ptr<void> addr) const {
  if (addr + page_size() < addr) {
    // Assume the last byte in the address space is never mapped; avoid overflow
    return false;
  }
  MemoryRange m(floor_page_size(addr), page_size());
  auto it = mem.find(m);
  return it != mem.end() && it->first.contains(m);
}

void AddressSpace::protect(remote_ptr<void> addr, size_t num_bytes, int prot) {
  LOG(debug) << "mprotect(" << addr << ", " << num_bytes << ", " << HEX(prot)
             << ")";

  MemoryRange last_overlap;
  auto protector = [this, prot, &last_overlap](const Mapping& mm,
                                               const MemoryRange& rem) {
    LOG(debug) << "  protecting (" << rem << ") ...";

    Mapping m = move(mm);
    mem.erase(m.map);

    // PROT_GROWSDOWN means that if this is a grows-down segment
    // (which for us means "stack") then the change should be
    // extended to the start of the segment.
    // We don't try to handle the analogous PROT_GROWSUP, because we
    // don't understand the idea of a grows-up segment.
    remote_ptr<void> new_start;
    if ((m.map.start() < rem.start()) && (prot & PROT_GROWSDOWN)) {
      new_start = m.map.start();
      LOG(debug) << "  PROT_GROWSDOWN: expanded region down to " << new_start;
    } else {
      new_start = rem.start();
    }

    LOG(debug) << "  erased (" << m.map << ")";

    // If the first segment we protect underflows the
    // region, remap the underflow region with previous
    // prot.
    if (m.map.start() < new_start) {
      Mapping underflow(
          m.map.subrange(m.map.start(), rem.start()),
          m.recorded_map.subrange(m.recorded_map.start(), rem.start()),
          m.emu_file);
      mem[underflow.map] = underflow;
    }
    // Remap the overlapping region with the new prot.
    remote_ptr<void> new_end = min(rem.end(), m.map.end());

    int new_prot = prot & (PROT_READ | PROT_WRITE | PROT_EXEC);
    Mapping overlap(
        m.map.subrange(new_start, new_end).set_prot(new_prot),
        m.recorded_map.subrange(new_start, new_end).set_prot(new_prot),
        m.emu_file);
    mem[overlap.map] = overlap;
    last_overlap = overlap.map;

    // If the last segment we protect overflows the
    // region, remap the overflow region with previous
    // prot.
    if (rem.end() < m.map.end()) {
      Mapping overflow(m.map.subrange(rem.end(), m.map.end()),
                       m.recorded_map.subrange(rem.end(), m.map.end()),
                       m.emu_file);
      mem[overflow.map] = overflow;
    }
  };
  for_each_in_range(addr, num_bytes, protector, ITERATE_CONTIGUOUS);
  if (last_overlap.size()) {
    // All mappings that we altered which might need coalescing
    // are adjacent to |last_overlap|.
    coalesce_around(mem.find(last_overlap));
  }
}

void AddressSpace::fixup_mprotect_growsdown_parameters(Task* t) {
  ASSERT(t, !(t->regs().arg3() & PROT_GROWSUP));
  if (t->regs().arg3() & PROT_GROWSDOWN) {
    Registers r = t->regs();
    if (r.arg1() == floor_page_size(r.arg1()) && has_mapping(r.arg1())) {
      auto& km = mapping_of(r.arg1()).map;
      if (km.flags() & MAP_GROWSDOWN) {
        auto new_start = km.start();
        r.set_arg2(remote_ptr<void>(r.arg1()) + size_t(r.arg2()) - new_start);
        r.set_arg1(new_start);
        r.set_arg3(r.arg3() & ~PROT_GROWSDOWN);
        t->set_regs(r);
      }
    }
  }
}

void AddressSpace::remap(remote_ptr<void> old_addr, size_t old_num_bytes,
                         remote_ptr<void> new_addr, size_t new_num_bytes) {
  LOG(debug) << "mremap(" << old_addr << ", " << old_num_bytes << ", "
             << new_addr << ", " << new_num_bytes << ")";

  auto mr = mapping_of(old_addr);
  const KernelMapping& m = mr.map;

  old_num_bytes = ceil_page_size(old_num_bytes);
  unmap_internal(old_addr, old_num_bytes);
  if (0 == new_num_bytes) {
    return;
  }
  new_num_bytes = ceil_page_size(new_num_bytes);

  auto it = dont_fork.lower_bound(MemoryRange(old_addr, old_num_bytes));
  if (it != dont_fork.end() && it->start() < old_addr + old_num_bytes) {
    // mremap fails if some but not all pages are marked DONTFORK
    assert(*it == MemoryRange(old_addr, old_num_bytes));
    remove_range(dont_fork, MemoryRange(old_addr, old_num_bytes));
    add_range(dont_fork, MemoryRange(new_addr, new_num_bytes));
  } else {
    remove_range(dont_fork, MemoryRange(old_addr, old_num_bytes));
    remove_range(dont_fork, MemoryRange(new_addr, new_num_bytes));
  }

  remote_ptr<void> new_end = new_addr + new_num_bytes;
  map_and_coalesce(m.set_range(new_addr, new_end),
                   mr.recorded_map.set_range(new_addr, new_end), mr.emu_file);
}

void AddressSpace::remove_breakpoint(remote_code_ptr addr,
                                     BreakpointType type) {
  auto it = breakpoints.find(addr);
  if (it == breakpoints.end() || it->second.unref(type) > 0) {
    return;
  }
  destroy_breakpoint(it);
}

bool AddressSpace::add_breakpoint(remote_code_ptr addr, BreakpointType type) {
  auto it = breakpoints.find(addr);
  if (it == breakpoints.end()) {
    uint8_t overwritten_data;
    // Grab a random task from the VM so we can use its
    // read/write_mem() helpers.
    Task* t = *task_set().begin();
    if (sizeof(overwritten_data) !=
        t->read_bytes_fallible(addr.to_data_ptr<uint8_t>(),
                               sizeof(overwritten_data), &overwritten_data)) {
      return false;
    }
    t->write_mem(addr.to_data_ptr<uint8_t>(), breakpoint_insn);

    auto it_and_is_new = breakpoints.insert(make_pair(addr, Breakpoint()));
    assert(it_and_is_new.second);
    it_and_is_new.first->second.overwritten_data = overwritten_data;
    it = it_and_is_new.first;
  }
  it->second.ref(type);
  return true;
}

void AddressSpace::remove_all_breakpoints() {
  while (!breakpoints.empty()) {
    destroy_breakpoint(breakpoints.begin());
  }
}

void AddressSpace::suspend_breakpoint_at(remote_code_ptr addr) {
  auto it = breakpoints.find(addr);
  if (it != breakpoints.end()) {
    Task* t = *task_set().begin();
    t->write_mem(addr.to_data_ptr<uint8_t>(), it->second.overwritten_data);
  }
}

void AddressSpace::restore_breakpoint_at(remote_code_ptr addr) {
  auto it = breakpoints.find(addr);
  if (it != breakpoints.end()) {
    Task* t = *task_set().begin();
    t->write_mem(addr.to_data_ptr<uint8_t>(), breakpoint_insn);
  }
}

int AddressSpace::access_bits_of(WatchType type) {
  switch (type) {
    case WATCH_EXEC:
      return EXEC_BIT;
    case WATCH_WRITE:
      return WRITE_BIT;
    case WATCH_READWRITE:
      return READ_BIT | WRITE_BIT;
    default:
      FATAL() << "Unknown watchpoint type " << type;
      return 0; // not reached
  }
}

void AddressSpace::remove_watchpoint(remote_ptr<void> addr, size_t num_bytes,
                                     WatchType type) {
  auto it = watchpoints.find(MemoryRange(addr, num_bytes));
  if (it != watchpoints.end() &&
      0 == it->second.unwatch(access_bits_of(type))) {
    watchpoints.erase(it);
  }
  allocate_watchpoints();
}

bool AddressSpace::add_watchpoint(remote_ptr<void> addr, size_t num_bytes,
                                  WatchType type) {
  MemoryRange key(addr, num_bytes);
  auto it = watchpoints.find(key);
  if (it == watchpoints.end()) {
    auto it_and_is_new =
        watchpoints.insert(make_pair(key, Watchpoint(num_bytes)));
    assert(it_and_is_new.second);
    it = it_and_is_new.first;
    update_watchpoint_value(it->first, it->second);
  }
  it->second.watch(access_bits_of(type));
  return allocate_watchpoints();
}

void AddressSpace::save_watchpoints() {
  saved_watchpoints.push_back(watchpoints);
}

bool AddressSpace::restore_watchpoints() {
  assert(!saved_watchpoints.empty());
  watchpoints = saved_watchpoints[saved_watchpoints.size() - 1];
  saved_watchpoints.pop_back();
  return allocate_watchpoints();
}

bool AddressSpace::update_watchpoint_value(const MemoryRange& range,
                                           Watchpoint& watchpoint) {
  Task* t = *task_set().begin();
  bool valid = true;
  vector<uint8_t> value_bytes = watchpoint.value_bytes;
  for (size_t i = 0; i < value_bytes.size(); ++i) {
    value_bytes[i] = 0xFF;
  }
  remote_ptr<void> addr = range.start();
  size_t num_bytes = range.size();
  while (num_bytes > 0) {
    ssize_t bytes_read = t->read_bytes_fallible(
        addr, num_bytes, value_bytes.data() + (addr - range.start()));
    if (bytes_read <= 0) {
      valid = false;
      // advance to next page and try to read more. We want to know
      // when the valid part of a partially invalid watchpoint changes.
      bytes_read =
          min<size_t>(num_bytes, (floor_page_size(addr) + page_size()) - addr);
    }
    addr += bytes_read;
    num_bytes -= bytes_read;
  }

  bool changed = valid != watchpoint.valid ||
                 memcmp(value_bytes.data(), watchpoint.value_bytes.data(),
                        value_bytes.size()) != 0;
  watchpoint.valid = valid;
  watchpoint.value_bytes = value_bytes;
  return changed;
}

void AddressSpace::update_watchpoint_values(remote_ptr<void> start,
                                            remote_ptr<void> end) {
  MemoryRange r(start, end);
  for (auto& it : watchpoints) {
    if (it.first.intersects(r) &&
        update_watchpoint_value(it.first, it.second)) {
      it.second.changed = true;
      // We do nothing to track kernel reads of read-write watchpoints...
    }
  }
}

static int DR_WATCHPOINT(int n) { return 1 << n; }

static bool watchpoint_triggered(uintptr_t debug_status,
                                 const vector<int8_t>& regs) {
  for (auto reg : regs) {
    if (debug_status & DR_WATCHPOINT(reg)) {
      return true;
    }
  }
  return false;
}

bool AddressSpace::notify_watchpoint_fired(uintptr_t debug_status) {
  bool triggered = false;
  for (auto& it : watchpoints) {
    if (((it.second.watched_bits() & WRITE_BIT) &&
         update_watchpoint_value(it.first, it.second)) ||
        ((it.second.watched_bits() & (READ_BIT | EXEC_BIT)) &&
         watchpoint_triggered(debug_status,
                              it.second.debug_regs_for_exec_read))) {
      it.second.changed = true;
      triggered = true;
    }
  }
  return triggered;
}

void AddressSpace::notify_written(remote_ptr<void> addr, size_t num_bytes) {
  update_watchpoint_values(addr, addr + num_bytes);
  session()->accumulate_bytes_written(num_bytes);
}

void AddressSpace::remove_all_watchpoints() {
  watchpoints.clear();
  allocate_watchpoints();
}

void AddressSpace::unmap(remote_ptr<void> addr, ssize_t num_bytes) {
  LOG(debug) << "munmap(" << addr << ", " << num_bytes << ")";
  num_bytes = ceil_page_size(num_bytes);
  if (!num_bytes) {
    return;
  }

  remove_range(dont_fork, MemoryRange(addr, num_bytes));

  return unmap_internal(addr, num_bytes);
}

void AddressSpace::unmap_internal(remote_ptr<void> addr, ssize_t num_bytes) {
  LOG(debug) << "munmap(" << addr << ", " << num_bytes << ")";

  auto unmapper = [this](const Mapping& mm, const MemoryRange& rem) {
    LOG(debug) << "  unmapping (" << rem << ") ...";

    Mapping m = move(mm);
    mem.erase(m.map);
    LOG(debug) << "  erased (" << m.map << ") ...";

    // If the first segment we unmap underflows the unmap
    // region, remap the underflow region.
    if (m.map.start() < rem.start()) {
      Mapping underflow(m.map.subrange(m.map.start(), rem.start()),
                        m.recorded_map.subrange(m.map.start(), rem.start()),
                        m.emu_file);
      mem[underflow.map] = underflow;
    }
    // If the last segment we unmap overflows the unmap
    // region, remap the overflow region.
    if (rem.end() < m.map.end()) {
      Mapping overflow(m.map.subrange(rem.end(), m.map.end()),
                       m.recorded_map.subrange(rem.end(), m.map.end()),
                       m.emu_file);
      mem[overflow.map] = overflow;
    }
  };
  for_each_in_range(addr, num_bytes, unmapper);
  update_watchpoint_values(addr, addr + num_bytes);
}

void AddressSpace::advise(remote_ptr<void> addr, ssize_t num_bytes,
                          int advice) {
  LOG(debug) << "madvise(" << addr << ", " << num_bytes << ", " << advice
             << ")";
  num_bytes = ceil_page_size(num_bytes);

  switch (advice) {
    case MADV_DONTFORK:
      add_range(dont_fork, MemoryRange(addr, num_bytes));
      break;
    case MADV_DOFORK:
      remove_range(dont_fork, MemoryRange(addr, num_bytes));
      break;
    default:
      break;
  }
}

void AddressSpace::did_fork_into(Task* t) {
  for (auto& range : dont_fork) {
    // During recording we execute MADV_DONTFORK so the forked child will
    // have had its dontfork areas unmapped by the kernel already
    if (!t->session().is_recording()) {
      AutoRemoteSyscalls remote(t);
      remote.infallible_syscall(syscall_number_for_munmap(remote.arch()),
                                range.start(), range.size());
    }
    t->vm()->unmap(range.start(), range.size());
  }
}

static string strip_deleted(const string& s) {
  static const char deleted[] = " (deleted)";
  ssize_t find_deleted = s.size() - (sizeof(deleted) - 1);
  if (s.find(deleted) == size_t(find_deleted)) {
    return s.substr(0, find_deleted);
  }
  return s;
}

enum HandleHeap { TREAT_HEAP_AS_ANONYMOUS, RESPECT_HEAP };

static bool normalized_file_names_equal(const KernelMapping& km1,
                                        const KernelMapping& km2,
                                        HandleHeap handle_heap) {
  if (km1.is_stack() || km2.is_stack()) {
    // The kernel seems to use "[stack:<tid>]" for any mapping area containing
    // thread |tid|'s stack pointer. When the thread exits, the next read of
    // the maps doesn't treat the area as stack at all. We don't want to track
    // thread exits, so if one of the mappings is a stack, skip the name
    // comparison. Device and inode numbers will still be checked.
    return true;
  }
  if (handle_heap == TREAT_HEAP_AS_ANONYMOUS &&
      (km1.is_heap() || km2.is_heap())) {
    // The kernel's heuristics for treating an anonymous mapping as "[heap]"
    // are obscure. Just skip the name check. Device and inode numbers will
    // still be checked.
    return true;
  }
  // We don't track when a file gets deleted, so it's possible for the kernel
  // to have " (deleted)" when we don't.
  return strip_deleted(km1.fsname()) == strip_deleted(km2.fsname());
}

/**
 * Return true iff |left| and |right| are located adjacently in memory
 * with the same metadata, and map adjacent locations of the same
 * underlying (real) device.
 */
static bool is_adjacent_mapping(const KernelMapping& mleft,
                                const KernelMapping& mright,
                                HandleHeap handle_heap,
                                int32_t flags_to_check = 0xFFFFFFFF) {
  if (mleft.end() != mright.start()) {
    LOG(debug) << "    (not adjacent in memory)";
    return false;
  }
  if (((mleft.flags() ^ mright.flags()) & flags_to_check) ||
      mleft.prot() != mright.prot()) {
    LOG(debug) << "    (flags or prot differ)";
    return false;
  }
  if (!normalized_file_names_equal(mleft, mright, handle_heap)) {
    LOG(debug) << "    (not the same filename)";
    return false;
  }
  if (mleft.device() != mright.device() || mleft.inode() != mright.inode()) {
    LOG(debug) << "    (not the same device/inode)";
    return false;
  }
  if (mleft.is_real_device() &&
      mleft.file_offset_bytes() + off64_t(mleft.size()) !=
          mright.file_offset_bytes()) {
    LOG(debug) << "    (" << mleft.file_offset_bytes() << " + " << mleft.size()
               << " != " << mright.file_offset_bytes()
               << ": offsets into real device aren't adjacent)";
    return false;
  }
  LOG(debug) << "    adjacent!";
  return true;
}

/**
 * If |*left_m| and |right_m| are adjacent (see
 * |is_adjacent_mapping()|), write a merged segment descriptor to
 * |*left_m| and return true.  Otherwise return false.
 */
static bool try_merge_adjacent(KernelMapping* left_m,
                               const KernelMapping& right_m) {
  if (is_adjacent_mapping(*left_m, right_m, TREAT_HEAP_AS_ANONYMOUS,
                          KernelMapping::checkable_flags_mask)) {
    *left_m = KernelMapping(left_m->start(), right_m.end(), left_m->fsname(),
                            left_m->device(), left_m->inode(), right_m.prot(),
                            right_m.flags(), left_m->file_offset_bytes());
    return true;
  }
  return false;
}

static dev_t normalized_device_number(const KernelMapping& m) {
  if (m.fsname().c_str()[0] != '/') {
    return m.device();
  }
  // btrfs files can report the wrong device number in /proc/<pid>/maps, so
  // restrict ourselves to checking whether the device number is != 0
  if (m.device() != KernelMapping::NO_DEVICE) {
    return (dev_t)-1;
  }
  return m.device();
}

static void assert_segments_match(Task* t, const KernelMapping& input_m,
                                  const KernelMapping& km) {
  KernelMapping m = input_m;
  string err;
  if (m.start() != km.start()) {
    err = "starts differ";
  } else if (m.end() != km.end()) {
    err = "ends differ";
  } else if (m.prot() != km.prot()) {
    err = "prots differ";
  } else if ((m.flags() ^ km.flags()) & KernelMapping::checkable_flags_mask) {
    err = "flags differ";
  } else if (!normalized_file_names_equal(m, km, TREAT_HEAP_AS_ANONYMOUS) &&
             !(km.is_heap() && m.fsname() == "") &&
             !(m.is_heap() && km.fsname() == "") && !km.is_vdso()) {
    // Due to emulated exec, the kernel may identify any of our anonymous maps
    // as [heap] (or not).
    // Kernels before 3.16 have a bug where any mapping at the original VDSO
    // address is marked [vdso] even if the VDSO was unmapped and replaced by
    // something else, so if the kernel reports [vdso] it may be spurious and
    // we skip this check. See kernel commit
    // a62c34bd2a8a3f159945becd57401e478818d51c.
    err = "filenames differ";
  } else if (normalized_device_number(m) != normalized_device_number(km)) {
    err = "devices_differ";
  } else if (m.inode() != km.inode()) {
    err = "inodes differ";
  }
  if (err.size()) {
    LOG(error) << "cached mmap:";
    t->vm()->dump();
    LOG(error) << "/proc/" << t->tid << "/mmaps:";
    print_process_mmap(t);
    ASSERT(t, false) << "\nCached mapping " << m << " should be " << km << "; "
                     << err;
  }
}

KernelMapping AddressSpace::fix_stack_segment_start(
    const MemoryRange& mapping, remote_ptr<void> new_start) {
  auto it = mem.find(mapping);
  it->first.update_start(new_start);
  it->second.map.update_start(new_start);
  it->second.recorded_map.update_start(new_start);
  return it->second.map;
}

KernelMapping AddressSpace::vdso() const {
  assert(!vdso_start_addr.is_null());
  return mapping_of(vdso_start_addr).map;
}

/**
 * Iterate over /proc/maps segments for a task and verify that the
 * task's cached mapping matches the kernel's (given a lenient fuzz
 * factor).
 */
void AddressSpace::verify(Task* t) const {
  ASSERT(t, task_set().end() != task_set().find(t));

  MemoryMap::const_iterator mem_it = mem.begin();
  KernelMapIterator kernel_it(t);
  while (!kernel_it.at_end() && mem_it != mem.end()) {
    KernelMapping km = kernel_it.current();
    ++kernel_it;
    while (!kernel_it.at_end()) {
      KernelMapping next_km = kernel_it.current();
      if (!try_merge_adjacent(&km, next_km)) {
        break;
      }
      ++kernel_it;
    }

    KernelMapping vm = mem_it->second.map;
    ++mem_it;
    while (mem_it != mem.end() && try_merge_adjacent(&vm, mem_it->second.map)) {
      ++mem_it;
    }

    assert_segments_match(t, vm, km);
  }

  ASSERT(t, kernel_it.at_end() && mem_it == mem.end());
}

AddressSpace::AddressSpace(Task* t, const string& exe, uint32_t exec_count)
    : exe(exe),
      leader_tid_(t->rec_tid),
      leader_serial(t->tuid().serial()),
      exec_count(exec_count),
      is_clone(false),
      session_(&t->session()),
      monkeypatch_state(t->session().is_recording() ? new Monkeypatcher()
                                                    : nullptr),
      syscallbuf_enabled_(false),
      first_run_event_(0) {
  // TODO: this is a workaround of
  // https://github.com/mozilla/rr/issues/1113 .
  if (session_->done_initial_exec()) {
    populate_address_space(t);
    assert(!vdso_start_addr.is_null());
  } else {
    // Find the location of the VDSO in the just-spawned process. This will
    // match the VDSO in rr itself since we haven't execed yet. So, speed
    // things up by searching rr's own VDSO for a syscall instruction.
    size_t rr_vdso_len;
    remote_ptr<void> rr_vdso = find_rr_vdso(t, &rr_vdso_len);
    // Here we rely on the VDSO location in the spawned tracee being the same
    // as in rr itself.
    uint8_t* local_vdso = reinterpret_cast<uint8_t*>(rr_vdso.as_int());
    auto offset = find_offset_of_syscall_instruction_in(
        NativeArch::arch(), local_vdso, rr_vdso_len);
    offset_to_syscall_in_vdso[NativeArch::arch()] = offset;
    // Setup traced_syscall_ip_ now because we need to do AutoRemoteSyscalls
    // (for open_mem_fd) before the first exec.
    traced_syscall_ip_ = remote_code_ptr(rr_vdso.as_int() + offset);
  }
}

AddressSpace::AddressSpace(Session* session, const AddressSpace& o,
                           pid_t leader_tid, uint32_t leader_serial,
                           uint32_t exec_count)
    : exe(o.exe),
      leader_tid_(leader_tid),
      leader_serial(leader_serial),
      exec_count(exec_count),
      brk_start(o.brk_start),
      brk_end(o.brk_end),
      is_clone(true),
      mem(o.mem),
      session_(session),
      vdso_start_addr(o.vdso_start_addr),
      monkeypatch_state(o.monkeypatch_state
                            ? new Monkeypatcher(*o.monkeypatch_state)
                            : nullptr),
      traced_syscall_ip_(o.traced_syscall_ip_),
      privileged_traced_syscall_ip_(o.privileged_traced_syscall_ip_),
      syscallbuf_lib_start_(o.syscallbuf_lib_start_),
      syscallbuf_lib_end_(o.syscallbuf_lib_end_),
      syscallbuf_enabled_(o.syscallbuf_enabled_),
      saved_auxv_(o.saved_auxv_),
      first_run_event_(0) {
  for (auto& it : o.breakpoints) {
    breakpoints.insert(make_pair(it.first, it.second));
  }
  for (auto& it : o.watchpoints) {
    watchpoints.insert(make_pair(it.first, it.second));
  }
  if (session != o.session()) {
    // Cloning into a new session means we're checkpointing.
    first_run_event_ = o.first_run_event_;
  }
  // cloned tasks will automatically get cloned debug registers and
  // cloned address-space memory, so we don't need to do any more work here.
}

static bool try_split_unaligned_range(MemoryRange& range, size_t bytes,
                                      vector<MemoryRange>& result) {
  if ((range.start().as_int() & (bytes - 1)) || range.size() < bytes) {
    return false;
  }
  result.push_back(MemoryRange(range.start(), bytes));
  range = MemoryRange(range.start() + bytes, range.end());
  return true;
}

static vector<MemoryRange> split_range(const MemoryRange& range) {
  vector<MemoryRange> result;
  MemoryRange r = range;
  while (r.size() > 0) {
    if ((sizeof(void*) < 8 || !try_split_unaligned_range(r, 8, result)) &&
        !try_split_unaligned_range(r, 4, result) &&
        !try_split_unaligned_range(r, 2, result)) {
      bool ret = try_split_unaligned_range(r, 1, result);
      assert(ret);
    }
  }
  return result;
}

static void configure_watch_registers(vector<WatchConfig>& regs,
                                      const MemoryRange& range, WatchType type,
                                      vector<int8_t>* assigned_regs) {
  auto split_ranges = split_range(range);

  if (type == WATCH_WRITE && range.size() > 1) {
    // We can suppress spurious write-watchpoint triggerings by checking
    // whether memory values have changed. So we can sometimes conserve
    // debug registers by upgrading an unaligned range to an aligned range
    // of a larger size.
    uintptr_t align;
    if (range.size() <= 2) {
      align = 2;
    } else if (range.size() <= 4 || sizeof(void*) <= 4) {
      align = 4;
    } else {
      align = 8;
    }
    remote_ptr<void> aligned_start(range.start().as_int() & ~(align - 1));
    remote_ptr<void> aligned_end((range.end().as_int() + (align - 1)) &
                                 ~(align - 1));
    auto split = split_range(MemoryRange(aligned_start, aligned_end));
    // If the aligned range doesn't reduce register usage, use the original
    // split to avoid spurious triggerings
    if (split.size() < split_ranges.size()) {
      split_ranges = split;
    }
  }

  for (auto& r : split_ranges) {
    if (assigned_regs) {
      assigned_regs->push_back(regs.size());
    }
    regs.push_back(WatchConfig(r.start(), r.size(), type));
  }
}

vector<WatchConfig> AddressSpace::get_watch_configs(
    WillSetTaskState will_set_task_state) {
  vector<WatchConfig> result;
  for (auto& kv : watchpoints) {
    vector<int8_t>* assigned_regs = nullptr;
    if (will_set_task_state == SETTING_TASK_STATE) {
      kv.second.debug_regs_for_exec_read.clear();
      assigned_regs = &kv.second.debug_regs_for_exec_read;
    }
    const MemoryRange& r = kv.first;
    int watching = kv.second.watched_bits();
    if (EXEC_BIT & watching) {
      configure_watch_registers(result, r, WATCH_EXEC, assigned_regs);
    }
    if (READ_BIT & watching) {
      configure_watch_registers(result, r, WATCH_READWRITE, assigned_regs);
    } else if (WRITE_BIT & watching) {
      configure_watch_registers(result, r, WATCH_WRITE, nullptr);
    }
  }
  return result;
}

vector<WatchConfig> AddressSpace::get_watchpoints_internal(
    WatchpointFilter filter) {
  vector<WatchConfig> result;
  for (auto& kv : watchpoints) {
    if (filter == CHANGED_WATCHPOINTS) {
      if (!kv.second.changed) {
        continue;
      }
      kv.second.changed = false;
    }
    const MemoryRange& r = kv.first;
    int watching = kv.second.watched_bits();
    if (EXEC_BIT & watching) {
      result.push_back(WatchConfig(r.start(), r.size(), WATCH_EXEC));
    }
    if (READ_BIT & watching) {
      result.push_back(WatchConfig(r.start(), r.size(), WATCH_READWRITE));
    } else if (WRITE_BIT & watching) {
      result.push_back(WatchConfig(r.start(), r.size(), WATCH_WRITE));
    }
  }
  return result;
}

vector<WatchConfig> AddressSpace::consume_watchpoint_changes() {
  return get_watchpoints_internal(CHANGED_WATCHPOINTS);
}

vector<WatchConfig> AddressSpace::all_watchpoints() {
  return get_watchpoints_internal(ALL_WATCHPOINTS);
}

bool AddressSpace::has_any_watchpoint_changes() {
  for (auto& kv : watchpoints) {
    if (kv.second.changed) {
      return true;
    }
  }
  return false;
}

bool AddressSpace::has_exec_watchpoint_fired(remote_code_ptr addr) {
  for (auto& kv : watchpoints) {
    if (kv.second.changed && kv.second.exec_count > 0 &&
        kv.first.start() == addr.to_data_ptr<void>()) {
      return true;
    }
  }
  return false;
}

bool AddressSpace::allocate_watchpoints() {
  Task::DebugRegs regs = get_watch_configs(SETTING_TASK_STATE);

  if (regs.size() <= 0x7f) {
    bool ok = true;
    for (auto t : task_set()) {
      if (!t->set_debug_regs(regs)) {
        ok = false;
      }
    }
    if (ok) {
      return true;
    }
  }

  regs.clear();
  for (auto t2 : task_set()) {
    t2->set_debug_regs(regs);
  }
  for (auto kv : watchpoints) {
    kv.second.debug_regs_for_exec_read.clear();
  }
  return false;
}

void AddressSpace::coalesce_around(MemoryMap::iterator it) {
  auto first_kv = it;
  while (mem.begin() != first_kv) {
    auto next = first_kv;
    --first_kv;
    if (!is_adjacent_mapping(first_kv->second.map, next->second.map,
                             RESPECT_HEAP)) {
      first_kv = next;
      break;
    }
    assert(first_kv->second.emu_file == next->second.emu_file);
  }
  auto last_kv = it;
  while (true) {
    auto prev = last_kv;
    ++last_kv;
    if (mem.end() == last_kv ||
        !is_adjacent_mapping(prev->second.map, last_kv->second.map,
                             RESPECT_HEAP)) {
      last_kv = prev;
      break;
    }
    assert(prev->second.emu_file == last_kv->second.emu_file);
  }
  assert(last_kv != mem.end());
  if (first_kv == last_kv) {
    LOG(debug) << "  no mappings to coalesce";
    return;
  }

  Mapping new_m(first_kv->second.map.extend(last_kv->first.end()),
                first_kv->second.recorded_map.extend(last_kv->first.end()),
                first_kv->second.emu_file);
  LOG(debug) << "  coalescing " << new_m.map;

  mem.erase(first_kv, ++last_kv);

  auto ins = mem.insert(MemoryMap::value_type(new_m.map, new_m));
  assert(ins.second); // key didn't already exist
}

void AddressSpace::destroy_breakpoint(BreakpointMap::const_iterator it) {
  Task* t = *task_set().begin();
  t->write_mem(it->first.to_data_ptr<uint8_t>(), it->second.overwritten_data);
  breakpoints.erase(it);
}

void AddressSpace::for_each_in_range(
    remote_ptr<void> addr, ssize_t num_bytes,
    function<void(const Mapping& m, const MemoryRange& rem)> f, int how) {
  remote_ptr<void> region_start = floor_page_size(addr);
  remote_ptr<void> last_unmapped_end = region_start;
  remote_ptr<void> region_end = ceil_page_size(addr + num_bytes);
  while (last_unmapped_end < region_end) {
    // Invariant: |rem| is always exactly the region of
    // memory remaining to be examined for pages to be
    // unmapped.
    MemoryRange rem(last_unmapped_end, region_end);

    // The next page to iterate may not be contiguous with
    // the last one seen.
    auto it = mem.lower_bound(rem);
    if (mem.end() == it) {
      LOG(debug) << "  not found, done.";
      return;
    }

    // Don't make a reference here. |f| is allowed to erase Mappings.
    MemoryRange range = it->first;
    if (rem.end() <= range.start()) {
      LOG(debug) << "  mapping at " << range.start() << " out of range, done.";
      return;
    }
    if (ITERATE_CONTIGUOUS == how &&
        !(range.start() < region_start || rem.start() == range.start())) {
      LOG(debug) << "  discontiguous mapping at " << range.start() << ", done.";
      return;
    }

    f(it->second, rem);

    // Maintain the loop invariant.
    last_unmapped_end = range.end();
  }
}

void AddressSpace::map_and_coalesce(const KernelMapping& m,
                                    const KernelMapping& recorded_map,
                                    EmuFile::shr_ptr emu_file) {
  LOG(debug) << "  mapping " << m;

  auto ins =
      mem.insert(MemoryMap::value_type(m, Mapping(m, recorded_map, emu_file)));
  coalesce_around(ins.first);

  update_watchpoint_values(m.start(), m.end());
}

static bool could_be_stack(const KernelMapping& km) {
  // On 4.1.6-200.fc22.x86_64 we observe that during exec of the exec_stub
  // during replay, when the process switches from 32-bit to 64-bit, the 64-bit
  // registers seem truncated to 32 bits during the initial PTRACE_GETREGS so
  // our sp looks wrong and /proc/<pid>/maps doesn't identify the region as
  // stack.
  // On stub execs there should only be one read-writable memory area anyway.
  return km.prot() == (PROT_READ | PROT_WRITE) && km.fsname() == "" &&
         km.device() == KernelMapping::NO_DEVICE &&
         km.inode() == KernelMapping::NO_INODE;
}

static dev_t check_device(Task* t, const KernelMapping& km) {
  if (km.fsname().c_str()[0] != '/') {
    return km.device();
  }
  // btrfs files can return the wrong device number in /proc/<pid>/maps
  struct stat st;
  int ret = stat(km.fsname().c_str(), &st);
  ASSERT(t, ret == 0);
  return st.st_dev;
}

void AddressSpace::populate_address_space(Task* t) {
  bool found_proper_stack = false;
  for (KernelMapIterator it(t); !it.at_end(); ++it) {
    auto& km = it.current();
    if (km.is_stack()) {
      found_proper_stack = true;
    }
  }

  int found_stacks = 0;
  for (KernelMapIterator it(t); !it.at_end(); ++it) {
    auto& km = it.current();
    int flags = km.flags();
    remote_ptr<void> start = km.start();
    ASSERT(t, flags & MAP_PRIVATE);
    bool is_stack = found_proper_stack ? km.is_stack() : could_be_stack(km);
    if (is_stack) {
      ++found_stacks;
      flags |= MAP_GROWSDOWN;
      // MAP_GROWSDOWN segments really occupy one additional page before
      // the start address shown by /proc/<pid>/maps --- unless that page
      // is already occupied by another mapping.
      if (!has_mapping(start - page_size())) {
        start -= page_size();
      }
    }

    map(start, km.end() - start, km.prot(), flags, km.file_offset_bytes(),
        km.fsname(), check_device(t, km), km.inode(), nullptr);
  }
  ASSERT(t, found_stacks == 1);
}

static int random_addr_bits(SupportedArch arch) {
  switch (arch) {
    default:
      assert(0 && "Unknown architecture");
    case x86:
      return 32;
    // Current x86-64 systems have only 48 bits of virtual address space,
    // and only the bottom half is usable by user space
    case x86_64:
      return 47;
  }
}

static MemoryRange adjust_range_for_stack_growth(const KernelMapping& km) {
  remote_ptr<void> start = km.start();
  if (km.flags() & MAP_GROWSDOWN) {
    start = min(start, km.end() - AddressSpace::chaos_mode_min_stack_size());
  }
  return MemoryRange(start, km.end());
}

remote_ptr<void> AddressSpace::chaos_mode_find_free_memory(Task* t,
                                                           size_t len) {
  remote_ptr<void> addr;
  // Half the time, try to allocate at a completely random address. The other
  // half of the time, we'll try to allocate immediately before or after a
  // randomly chosen existing mapping.
  if (random() % 2) {
    int bits = random_addr_bits(t->arch());
    // Some of these addresses will not be mappable. That's fine, the
    // kernel will fall back to a valid address if the hint is not valid.
    uint64_t r = ((uint64_t)(uint32_t)random() << 32) | (uint32_t)random();
    addr = floor_page_size(remote_ptr<void>(r & ((uint64_t(1) << bits) - 1)));
  } else {
    ASSERT(t, !mem.empty());
    int map_index = random() % mem.size();
    int map_count = 0;
    for (const auto& m : maps()) {
      if (map_count == map_index) {
        addr = m.map.start();
        break;
      }
      ++map_count;
    }
  }

  // If there's a collision (which there always will be in the second case
  // above), either move the mapping forwards or backwards in memory until it
  // fits. Choose the direction randomly.
  int direction = (random() % 2) ? 1 : -1;
  while (true) {
    Maps m = maps_starting_at(addr);
    if (m.begin() == m.end()) {
      return addr;
    }
    MemoryRange range = adjust_range_for_stack_growth(m.begin()->map);
    if (range.start() >= addr + len) {
      // No overlap with an existing mapping; we're good!
      return addr;
    }
    if (direction == -1) {
      addr = range.start() - len;
    } else {
      addr = range.end();
    }
  }
}

} // namespace rr
