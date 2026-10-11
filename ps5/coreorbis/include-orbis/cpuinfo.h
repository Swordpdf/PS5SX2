#pragma once
// Orbis cpuinfo shim: fixed Zen2 topology (PS5 Pro 8C/16T, 64B L1 lines).
// Replaces 3rdparty/cpuinfo lib, which needs OS arch-detection missing on Orbis.
// Feature queries match hardware (poc1 CPUID: SSE4.1+AVX+AVX2+FMA+BMI, no AVX512).
#include <stdint.h>

#define CPUINFO_ABI

#ifdef __cplusplus
extern "C" {
#endif

enum cpuinfo_vendor
{
  cpuinfo_vendor_unknown = 0,
  cpuinfo_vendor_intel = 1,
  cpuinfo_vendor_amd = 2
};

enum cpuinfo_uarch
{
  cpuinfo_uarch_unknown = 0,
  cpuinfo_uarch_haswell = 0x00100208,
  cpuinfo_uarch_zen2 = 0x0020010A
};

struct cpuinfo_cache
{
  uint32_t line_size;
};

struct cpuinfo_cluster
{
  uint32_t processor_start;
  uint32_t processor_count;
  uint32_t core_start;
  uint32_t core_count;
  uint32_t cluster_id;
};

struct cpuinfo_core
{
  uint32_t processor_start;
  uint32_t processor_count;
  uint32_t core_id;
  const struct cpuinfo_cluster* cluster;
  enum cpuinfo_vendor vendor;
};

struct cpuinfo_uarch_info
{
  enum cpuinfo_uarch uarch;
};

struct cpuinfo_processor_cache
{
  const struct cpuinfo_cache* l1i;
  const struct cpuinfo_cache* l1d;
};

struct cpuinfo_processor
{
  uint32_t smt_id;
  const struct cpuinfo_cluster* cluster;
  struct cpuinfo_processor_cache cache;
};

bool cpuinfo_initialize(void);
const struct cpuinfo_processor* cpuinfo_get_processor(uint32_t index);
uint32_t cpuinfo_get_processors_count(void);
uint32_t cpuinfo_get_clusters_count(void);
const struct cpuinfo_cluster* cpuinfo_get_cluster(uint32_t index);
uint32_t cpuinfo_get_cores_count(void);
const struct cpuinfo_core* cpuinfo_get_core(uint32_t index);
uint32_t cpuinfo_get_uarchs_count(void);
const struct cpuinfo_uarch_info* cpuinfo_get_uarch(uint32_t index);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
// Feature queries used by VMManager/MultiISA startup checks.
static inline bool cpuinfo_has_x86_sse4_1(void)
{
  return true;
}
static inline bool cpuinfo_has_x86_avx(void)
{
  return true;
}
static inline bool cpuinfo_has_x86_avx2(void)
{
  return true;
}
static inline bool cpuinfo_has_x86_avx512f(void)
{
  return false;
}
static inline bool cpuinfo_has_x86_fma3(void)
{
  return true;
}
static inline bool cpuinfo_has_x86_bmi(void)
{
  return true;
}
static inline bool cpuinfo_has_x86_bmi2(void)
{
  return true;
}
#endif
