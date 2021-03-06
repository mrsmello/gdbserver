// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "decoder.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>

#include <map>

#include "bin/gdbserver/lib/debugger-utils/ktrace-reader.h"
#include "bin/gdbserver/lib/debugger-utils/util.h"
#include "bin/gdbserver/third_party/simple-pt/elf.h"
#include "bin/gdbserver/third_party/simple-pt/symtab.h"

#include "lib/ftl/files/path.h"
#include "lib/ftl/files/unique_fd.h"
#include "lib/ftl/functional/auto_call.h"
#include "lib/ftl/logging.h"
#include "lib/ftl/strings/string_printf.h"

namespace intel_processor_trace {

using debugserver::util::ErrnoString;

// For passing data from ReadKtraceFile to ProcessKtraceRecord.
struct KtraceData {
  DecoderState* state;
};

int DecoderState::ProcessKtraceRecord(debugserver::ktrace::KtraceRecord* rec,
                                      void* arg) {
  KtraceData* data = reinterpret_cast<KtraceData*>(arg);
  DecoderState* state = data->state;

  // We're interested in TAG_IPT_* records.

  switch (rec->hdr.tag) {
    case TAG_IPT_START: {
      // N.B. There may be many IPT_START/STOP records present.
      // We only want the last one.
      const ktrace_rec_32b* r = &rec->r_32B;
      uint64_t kernel_cr3 = r->c | ((uint64_t)r->d << 32);
      state->set_nom_freq(r->a);
      state->set_kernel_cr3(kernel_cr3);
      FTL_LOG(INFO) << ftl::StringPrintf("Ktrace IPT start, ts %" PRIu64
                                         ", nom_freq %u, kernel cr3 0x%" PRIx64,
                                         rec->hdr.ts, r->a, kernel_cr3);
      break;
    }
    case TAG_IPT_CPU_INFO: {
      FTL_LOG(INFO) << "Ktrace IPT start, ts " << rec->hdr.ts;
      const ktrace_rec_32b* r = &rec->r_32B;
      state->set_family(r->b);
      state->set_model(r->c);
      state->set_stepping(r->d);
      FTL_LOG(INFO) << ftl::StringPrintf("Ktrace IPT CPU INFO, ts %" PRIu64
                                         ", family %u, model %u, stepping %u",
                                         rec->hdr.ts, r->b, r->c, r->d);
      break;
    }
    case TAG_IPT_STOP:
      FTL_LOG(INFO) << "Ktrace IPT stop, ts " << rec->hdr.ts;
      break;
    case TAG_IPT_PROCESS_CREATE: {
      const ktrace_rec_32b* r = &rec->r_32B;
      uint64_t pid = r->a | ((uint64_t)r->b << 32);
      uint64_t cr3 = r->c | ((uint64_t)r->d << 32);
      FTL_LOG(INFO) << ftl::StringPrintf("Ktrace process create, ts %" PRIu64
                                         ", pid %" PRIu64 ", cr3 0x%" PRIx64,
                                         rec->hdr.ts, pid, cr3);
      if (!state->AddProcess(pid, cr3, rec->hdr.ts)) {
        FTL_LOG(ERROR) << "Error adding process: " << pid;
      }
      break;
    }
    case TAG_PROC_EXIT: {
      const ktrace_rec_32b* r = &rec->r_32B;
      uint64_t pid = r->a | ((uint64_t)r->b << 32);
      FTL_LOG(INFO) << ftl::StringPrintf("Ktrace process exit, ts %" PRIu64
                                         ", pid %" PRIu64,
                                         rec->hdr.ts, pid);
      // N.B. We don't remove the process from any table here. This pass is run
      // before we scan the actual PT dump.
      if (!state->MarkProcessExited(pid, rec->hdr.ts)) {
        FTL_LOG(ERROR) << "Error marking process exit: " << pid;
      }
      break;
    }
  }

  return 0;
}

bool DecoderState::ReadKtraceFile(const std::string& file) {
  FTL_LOG(INFO) << "Loading ktrace data from " << file;

  ftl::UniqueFD fd(open(file.c_str(), O_RDONLY));
  if (!fd.is_valid()) {
    FTL_LOG(ERROR) << "error opening ktrace file" << ", " << ErrnoString(errno);
    return false;
  }

  KtraceData data = {this};
  int rc = debugserver::ktrace::ReadFile(fd.get(), ProcessKtraceRecord, &data);
  if (rc != 0) {
    FTL_LOG(ERROR) << ftl::StringPrintf("Error %d reading ktrace file", rc);
    return false;
  }

  return true;
}

bool DecoderState::ReadMapFile(const std::string& file) {
  // TODO(dje): This is temporary until we can properly trace ld.so activity.
  // MG-519
  return load_maps_.ReadLogListenerOutput(file);
}

bool DecoderState::ReadIdsFile(const std::string& file) {
  return build_ids_.ReadIdsFile(file);
}

bool DecoderState::ReadPtListFile(const std::string& file) {
  FTL_LOG(INFO) << "Loading pt file list from " << file;

  FILE* f = fopen(file.c_str(), "r");
  if (!f) {
    FTL_LOG(ERROR) << "error opening pt file list file" << ", "
                   << ErrnoString(errno);
    return false;
  }

  char* line = nullptr;
  size_t linelen = 0;
  int lineno = 1;

  auto cleanup = ftl::MakeAutoCall([line, f]() {
    free(line);
    fclose(f);
  });

  for (; getline(&line, &linelen, f) > 0; ++lineno) {
    size_t n = strlen(line);
    if (n > 0 && line[n - 1] == '\n')
      line[n - 1] = '\0';
    FTL_VLOG(2) << ftl::StringPrintf("read %d: %s", lineno, line);

#define MAX_LINE_LEN 1024
    if (linelen > MAX_LINE_LEN) {
      FTL_VLOG(2) << ftl::StringPrintf("%d: ignoring: %s", lineno, line);
      continue;
    }

    if (!strcmp(line, "\n"))
      continue;
    if (line[0] == '#')
      continue;

    unsigned long long id;
    char path[linelen];
    if (sscanf(line, "%llu %s", &id, path) == 2) {
      AddPtFile(files::GetDirectoryName(file), id, path);
    } else {
      FTL_VLOG(2) << ftl::StringPrintf("%d: ignoring: %s", lineno, line);
    }
  }

  return true;
}

void DecoderState::AddSymtab(std::unique_ptr<SymbolTable> symtab) {
  symtabs_.push_back(std::move(symtab));
}

bool DecoderState::ReadElf(const std::string& file_name,
                           uint64_t base,
                           uint64_t cr3,
                           uint64_t file_off,
                           uint64_t map_len) {
  FTL_DCHECK(image_);

  std::unique_ptr<SymbolTable> symtab;
  std::unique_ptr<SymbolTable> dynsym;

  if (!simple_pt::ReadElf(file_name.c_str(), image_, base, cr3,
                          file_off, map_len, &symtab, &dynsym))
    return false;

  if (symtab)
    AddSymtab(std::move(symtab));
  if (dynsym)
    AddSymtab(std::move(dynsym));
  return true;
}

bool DecoderState::ReadKernelElf(const std::string& file_name, uint64_t cr3) {
  FTL_DCHECK(image_);

  std::unique_ptr<SymbolTable> symtab;
  std::unique_ptr<SymbolTable> dynsym;

  if (!simple_pt::ReadNonPicElf(file_name.c_str(), image_, cr3, true,
                                &symtab, &dynsym))
    return false;

  if (symtab)
    AddSymtab(std::move(symtab));
  if (dynsym)
    FTL_LOG(WARNING) << "Kernel has SHT_DYNSYM symtab?";
  return true;
}

}  // namespace intel_processor_trace
