#include <iostream>
#include "stdio.h"
#include "common.h"
#include "dnnmark.h"
#include "usage.h"
#include <gflags/gflags.h>
#include "simpleCUBLAS.h"

using namespace dnnmark;

int main(int argc, char **argv) {
  INIT_FLAGS(argc, argv);
  INIT_LOG(argv);
  LOG(INFO) << "DNNMark suites: Start...";
  DNNMark<TestType> dnnmark(3);
  dnnmark.ParseAllConfig(FLAGS_config);

  if (FLAGS_warmup) {
    LOG(INFO) << "Warming up before initialisation...";
    for (int i = 0; i < 5; i++) {
      int status = call_sgemm(0, 512);
      if (status != 0) {
        fprintf(stderr, "Error status: %d\n",status);
      }
    }
  }
  dnnmark.Initialize();
  if (FLAGS_warmup) {
    for (int i = 0; i < 5; i++) {
      LOG(INFO) << "Warming up...";
      dnnmark.Forward();
      dnnmark.Backward();
    }
  }
  dnnmark.GetTimer()->Clear();

  // Real benchmark
  for (int i = 0; i < FLAGS_iterations; i++) {
    LOG(INFO) << "Iteration " << i;
    dnnmark.Forward();
    dnnmark.Backward();
  }
  dnnmark.GetTimer()->SumRecords();
  dnnmark.TearDown();

  LOG(INFO) << "Total running time(ms): " << dnnmark.GetTimer()->GetTotalTime();
  printf("Total running time(ms): %f\n", dnnmark.GetTimer()->GetTotalTime());
  LOG(INFO) << "DNNMark suites: Tear down...";
  return 0;
}
