// Fixed Zen2 topology for Orbis (matches poc2: ncpu=16, 64B lines).
// Single cluster: thread-placement code still works, just never migrates.
#include "cpuinfo.h"

static const struct cpuinfo_cache s_l1i = {64};
static const struct cpuinfo_cache s_l1d = {64};
static const struct cpuinfo_cluster s_cluster = {0, 16, 0, 8, 0};
static struct cpuinfo_processor s_procs[16];
static struct cpuinfo_core s_cores[8];
static const struct cpuinfo_uarch_info s_uarch = {cpuinfo_uarch_zen2};

extern "C" bool cpuinfo_initialize(void)
{
  for (int i = 0; i < 16; i++)
  {
    s_procs[i].smt_id = (uint32_t)(i & 1);
    s_procs[i].cluster = &s_cluster;
    s_procs[i].cache.l1i = &s_l1i;
    s_procs[i].cache.l1d = &s_l1d;
  }
  for (int i = 0; i < 8; i++)
  {
    s_cores[i].processor_start = (uint32_t)(i * 2);
    s_cores[i].processor_count = 2;
    s_cores[i].core_id = (uint32_t)i;
    s_cores[i].cluster = &s_cluster;
    s_cores[i].vendor = cpuinfo_vendor_amd;
  }
  return true;
}

extern "C" const struct cpuinfo_processor* cpuinfo_get_processor(uint32_t index)
{
  if (index >= 16)
    return 0;
  return &s_procs[index];
}

extern "C" uint32_t cpuinfo_get_processors_count(void)
{
  return 16;
}

extern "C" uint32_t cpuinfo_get_clusters_count(void)
{
  return 1;
}

extern "C" const struct cpuinfo_cluster* cpuinfo_get_cluster(uint32_t index)
{
  if (index != 0)
    return 0;
  return &s_cluster;
}

extern "C" uint32_t cpuinfo_get_cores_count(void)
{
  return 8;
}

extern "C" const struct cpuinfo_core* cpuinfo_get_core(uint32_t index)
{
  if (index >= 8)
    return 0;
  return &s_cores[index];
}

extern "C" uint32_t cpuinfo_get_uarchs_count(void)
{
  return 1;
}

extern "C" const struct cpuinfo_uarch_info* cpuinfo_get_uarch(uint32_t index)
{
  if (index != 0)
    return 0;
  return &s_uarch;
}
