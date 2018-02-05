// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2016, gperftools Contributors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#include "config.h"

#include "malloc_tracer.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "base/googleinit.h"
#include "base/spinlock.h"
#include "internal_logging.h"
#include "page_heap_allocator.h"
#include "tracer_buffer.h"

namespace tcmalloc {

static const int kDumperPeriodMicros = 3000;

static const int kTokenSize = 1 << 10;

static SpinLock lock(tcmalloc::LINKER_INITIALIZED);

static uint64_t token_counter;
static uint64_t thread_id_counter;
static uint64_t thread_dump_written;

static uint64_t base_ts;

__thread MallocTracer::Storage MallocTracer::instance_ ATTR_INITIAL_EXEC;
__thread bool had_tracer;

MallocTracer::Storage *MallocTracer::all_tracers_;

static pthread_key_t instance_key;
static pthread_once_t setup_once = PTHREAD_ONCE_INIT;
static pthread_once_t first_tracer_setup_once = PTHREAD_ONCE_INIT;

static tcmalloc::PageHeapAllocator<MallocTracer> malloc_tracer_allocator;

static TracerBuffer* tracer_buffer;

static bool no_more_writes;

COMPILE_ASSERT(sizeof(MallocTracer) == 4096, malloc_tracer_is_4k);

static union {
  struct {
    void *a, *b;
  } s;
  char space[sizeof(MallocTracer) + sizeof(void*)];
} first_tracer_space;

static MallocTracer *get_first_tracer() {
  return reinterpret_cast<MallocTracer *>(&first_tracer_space.s);
}

void MallocTracer::MallocTracerDestructor(void *arg) {
  CHECK_CONDITION(!had_tracer);

  MallocTracer::Storage *instanceptr =
      reinterpret_cast<MallocTracer::Storage *>(arg);

  MallocTracer *tracer = instanceptr->ptr;

  // have pthread call us again on next destruction iteration and give
  // rest of tls destructors chance to get traced properly
  if (tracer->destroy_count_++ < 3) {
    pthread_setspecific(instance_key, instanceptr);
    return;
  }

  if (instanceptr->pprev) {
    SpinLockHolder l(&lock);
    MallocTracer::Storage *s = *instanceptr->pprev = instanceptr->next;
    if (s) {
      s->pprev = instanceptr->pprev;
    }
    instanceptr->pprev =
        reinterpret_cast<MallocTracer::Storage **>(0xababababababababULL);
    instanceptr->next =
        reinterpret_cast<MallocTracer::Storage *>(0xcdcdcdcdcdcdcdcdULL);
  }

  had_tracer = true;
  instanceptr->ptr = NULL;
  tracer->~MallocTracer();

  if (tracer == get_first_tracer()) {
    return;
  }

  SpinLockHolder h(&lock);
  malloc_tracer_allocator.Delete(tracer);
}

static uint64_t get_nanos() {
  struct timespec tss;
  clock_gettime(CLOCK_MONOTONIC, &tss);
  return (uint64_t)tss.tv_sec * 1000000000 + tss.tv_nsec - base_ts;
}

void MallocTracer::SetupFirstTracer() {
  base_ts = get_nanos() & MallocTraceEncoder::kTSMask;
  new (get_first_tracer()) MallocTracer(0);
}

// in_setup guards cases of malloc calls during DoSetupTLS
static __thread bool in_setup ATTR_INITIAL_EXEC;

void MallocTracer::DoSetupTLS() {
  in_setup = true;

  tracer_buffer = TracerBuffer::GetInstance();

  uint32_t magic = MallocTraceEncoder::kMagic;
  tracer_buffer->AppendData(reinterpret_cast<char*>(&magic), sizeof(magic));

  malloc_tracer_allocator.Init();
  int rv = pthread_key_create(&instance_key,
                              &MallocTracer::MallocTracerDestructor);
  CHECK_CONDITION(!rv);

  in_setup = false;
}

static void *dumper_thread(void *__dummy) {
  while (true) {
    usleep(kDumperPeriodMicros);
    MallocTracer::DumpEverything();
  }
  return NULL;
}

static void malloc_tracer_setup_tail() {
  (void)MallocTracer::GetInstance();

  pthread_t dumper;
  int rv = pthread_create(&dumper, 0, dumper_thread, 0);
  if (rv != 0) {
    errno = rv;
    perror("pthread_create");
    CHECK_CONDITION(rv == 0);
  }
}

REGISTER_MODULE_INITIALIZER(setup_tail, malloc_tracer_setup_tail());

MallocTracer *MallocTracer::GetInstanceSlow(void) {
  pthread_once(&first_tracer_setup_once, MallocTracer::SetupFirstTracer);
  if (in_setup) {
    return get_first_tracer();
  }

  pthread_once(&setup_once, &MallocTracer::DoSetupTLS);

  MallocTracer *an_instance;
  {
    SpinLockHolder h(&lock);
    uint64_t thread_id = !had_tracer ? ++thread_id_counter : 0;

    if (thread_id == 1) {
      an_instance = get_first_tracer();
    } else {
      an_instance = malloc_tracer_allocator.New();
      new (an_instance) MallocTracer(thread_id);
    }

    instance_.ptr = an_instance;
    instance_.next = all_tracers_;
    instance_.pprev = &all_tracers_;

    if (instance_.next) {
      instance_.next->pprev = &instance_.next;
    }
    all_tracers_ = &instance_;
  }

  if (!had_tracer) {
    pthread_setspecific(instance_key, &instance_);
  }

  return an_instance;
}

MallocTracer::MallocTracer(uint64_t thread_id) {
  buf_ptr_ = buf_storage_;
  buf_end_ = buf_storage_ + sizeof(buf_storage_) - AltVarintCodec::kMaxSize;

  thread_id_ = thread_id;
  token_base_ = counter_ = 0;
  prev_size_ = 0;
  prev_token_ = 0;
  last_cpu_ = -1;
  signal_snapshot_buf_ptr_ = signal_saved_buf_ptr_ = buf_storage_;
  destroy_count_ = 0;

  RefreshToken();
}

static void append_buf_locked(const char *buf, size_t size) {
  if (no_more_writes) {
    return;
  }
  tracer_buffer->AppendData(buf, size);
}

uint64_t MallocTracer::UpdateTSAndCPU() {
  uint64_t ts = get_nanos();
  last_cpu_ = sched_getcpu();
  return MallocTraceEncoder::bundle_ts_and_cpu(ts, last_cpu_);
}

void MallocTracer::RefreshBufferInnerLocked(uint64_t size, uint64_t ts_and_cpu) {
  char meta_buf[32];
  char *p = meta_buf;
  MallocTraceEncoder::triple enc =
      MallocTraceEncoder::encode_buffer(thread_id_, ts_and_cpu, size);
  p = AltVarintCodec::encode_unsigned(p, enc.first);
  p = AltVarintCodec::encode_unsigned(p, enc.second.first);
  p = AltVarintCodec::encode_unsigned(p, enc.second.second);

  append_buf_locked(meta_buf, p - meta_buf);
  append_buf_locked(signal_saved_buf_ptr_, size);
}

void MallocTracer::RefreshBuffer() {
  SpinLockHolder h(&lock);

  if (buf_ptr_ != signal_saved_buf_ptr_) {
    RefreshBufferInnerLocked(buf_ptr_ - signal_saved_buf_ptr_,
                             UpdateTSAndCPU());
  }

  SetBufPtr(buf_storage_);
  signal_saved_buf_ptr_ = buf_storage_;
}

void MallocTracer::DumpFromSaverThread() {
  uint64_t s = signal_snapshot_buf_ptr_ - signal_saved_buf_ptr_;

  if (s == 0) {
    return;
  }

  uint64_t tscpu = MallocTraceEncoder::bundle_ts_and_cpu(get_nanos(), last_cpu_);
  RefreshBufferInnerLocked(s, tscpu);

  signal_saved_buf_ptr_ = signal_snapshot_buf_ptr_;

  thread_dump_written += s;
}

void MallocTracer::RefreshTokenAndDec() {
  uint64_t base = __sync_add_and_fetch(&token_counter, kTokenSize);

  token_base_ = base;
  counter_ = kTokenSize;

  MallocTraceEncoder::pair enc = MallocTraceEncoder::encode_token(
      base - kTokenSize,
      UpdateTSAndCPU());

  AppendWords(2, enc.first, enc.second);
}

void MallocTracer::RefreshToken() {
  RefreshTokenAndDec();
  counter_++;
}

static void process_wide_barrier() {
  // TODO: use membarrier or google-only rseq barrier
  // syscall
  static volatile char a_page[4096] __attribute__((aligned(4096)));
  // first touch page
  a_page[0] &= 0xff;
  // and then tell it to go away. This will trigger IPI to all cores
  // running this process' mm for tlb flush and wait for
  // completion. Causing memory barriers everywhere.
  madvise(const_cast<char*>(a_page), 4096, MADV_DONTNEED);
}

void MallocTracer::DumpEverything() {
  if (!tracer_buffer->IsFullySetup()) {
    return;
  }

  SpinLockHolder h(&lock);

  for (MallocTracer::Storage *s = all_tracers_; s != NULL; s = s->next) {
    // benign race reading buf_ptr here.
    char* buf_ptr = *const_cast<char * volatile *>(&s->ptr->buf_ptr_);
    s->ptr->signal_snapshot_buf_ptr_ = buf_ptr;
  }

  // ensure that we're able to see all the data written up to
  // signal_snapshot_buf_ptr of all threads
  process_wide_barrier();

  for (MallocTracer::Storage *s = all_tracers_; s != NULL; s = s->next) {
    if (s->ptr->signal_snapshot_buf_ptr_ == s->ptr->signal_saved_buf_ptr_) {
      continue;
    }
    s->ptr->DumpFromSaverThread();
  }

  char sync_end_buf[24];
  char *p = sync_end_buf;

  uint64_t ts_and_cpu = MallocTraceEncoder::bundle_ts_and_cpu(get_nanos(),
                                                              sched_getcpu());
  MallocTraceEncoder::pair enc =
      MallocTraceEncoder::encode_sync_barrier(ts_and_cpu);
  p = AltVarintCodec::encode_unsigned(p, enc.first);
  p = AltVarintCodec::encode_unsigned(p, enc.second);
  append_buf_locked(sync_end_buf, p - sync_end_buf);
}

void MallocTracer::ExcludeCurrentThreadFromDumping() {
  (void)GetInstance();

  if (instance_.pprev == NULL) {
    return;
  }

  SpinLockHolder h(&lock);
  *instance_.pprev = instance_.next;
  instance_.pprev = NULL;
}

MallocTracer::~MallocTracer() {
  RefreshBuffer();

  char *p = buf_ptr_;
  MallocTraceEncoder::pair enc =
      MallocTraceEncoder::encode_death(thread_id_, UpdateTSAndCPU());
  p = AltVarintCodec::encode_unsigned(p, enc.first);
  p = AltVarintCodec::encode_unsigned(p, enc.second);

  {
    SpinLockHolder h(&lock);
    append_buf_locked(buf_storage_, p - buf_storage_);
  }

  memset(this, 0xfe, sizeof(*this));
}

static void finalize_tracing() {
  // saving rest of trace may still malloc, particularly if saver
  // thread uses snappy. So we need to drop lock soon. But we drop all
  // further buffer writes.
  {
    SpinLockHolder h(&lock);
    no_more_writes = true;
  }

  char encoded_end[16];
  char *p = encoded_end;
  p = AltVarintCodec::encode_unsigned(p, MallocTraceEncoder::encode_end());
  ASSERT(p <= encoded_end + sizeof(encoded_end));

  tracer_buffer->AppendData(encoded_end, p - encoded_end);
  tracer_buffer->Finalize();
}

class TracerDeinit {
 public:
  ~TracerDeinit() {
    finalize_tracing();
  }
};
static TracerDeinit tracer_deinit;

void MallocTracer::SPrintStats(char* start, char* end) {
  snprintf(start, end - start,
           "token_counter = %llu\n"
           "thread_id_counter = %llu\n"
           "thread_dump_written = %llu\n",
           (unsigned long long)token_counter,
           (unsigned long long)thread_id_counter,
           (unsigned long long)thread_dump_written);
}

} // namespace tcmalloc
