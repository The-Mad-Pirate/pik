// Copyright 2019 Google LLC
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

#include "dpik.h"

#undef PROFILER_ENABLED
#define PROFILER_ENABLED 1

#include "cmdline.h"
#include "file_io.h"
#include "os_specific.h"
#include "padded_bytes.h"
#include "profiler.h"

namespace pik {
namespace {

int DecompressMain(int argc, char* argv[]) {
  DecompressArgs args;
  tools::CommandLineParser cmdline;
  args.AddCommandLineOptions(&cmdline);
  if (!cmdline.Parse(argc, argv) || !args.ValidateArgs()) {
    cmdline.PrintHelp();
    return 1;
  }

  const int bits = TargetBitfield().Bits();
  if ((bits & SIMD_ENABLE) != SIMD_ENABLE) {
    fprintf(stderr, "CPU does not support all enabled targets => exiting.\n");
    return 1;
  }

  PaddedBytes compressed;
  if (!ReadFile(args.file_in, &compressed)) return 1;
  fprintf(stderr, "Read %zu compressed bytes\n", compressed.size());

  CodecContext codec_context;
  ThreadPool pool(args.num_threads);
  DecompressStats stats;

  const std::vector<int> cpus = AvailableCPUs();
  pool.RunOnEachThread([&cpus](const int task, const int thread) {
    // 1.1-1.2x speedup (36 cores) from pinning.
    if (thread < cpus.size()) {
      if (!PinThreadToCPU(cpus[thread])) {
        fprintf(stderr, "WARNING: failed to pin thread %d.\n", thread);
      }
    }
  });

  CodecInOut io(&codec_context);
  for (size_t i = 0; i < args.num_reps; ++i) {
    if (!Decompress(&codec_context, compressed, args.params, &pool, &io,
                    &stats)) {
      return 1;
    }
  }

  if (!WriteOutput(args, io)) return 1;

  (void)stats.Print(io, &pool);

  if (args.print_profile == Override::kOn) {
    PROFILER_PRINT_RESULTS();
  }

  return 0;
}

}  // namespace
}  // namespace pik

int main(int argc, char* argv[]) { return pik::DecompressMain(argc, argv); }
