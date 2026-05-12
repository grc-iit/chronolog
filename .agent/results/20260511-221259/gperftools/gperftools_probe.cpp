#include <gperftools/heap-profiler.h>
#include <gperftools/profiler.h>
#include <cstdlib>
#include <vector>

int main() {
  ProfilerStart("cpu.prof");
  HeapProfilerStart("heap.prof");
  std::vector<void*> allocations;
  for (int i = 0; i < 1000; ++i) {
    allocations.push_back(std::malloc(1024));
  }
  HeapProfilerDump("after_alloc");
  for (void* p : allocations) {
    std::free(p);
  }
  HeapProfilerStop();
  ProfilerStop();
  return 0;
}
