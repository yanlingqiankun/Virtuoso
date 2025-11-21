#ifndef CORE_H
#define CORE_H
#include "semaphore.h"
#include "shmem_msg.h"

// some forward declarations for cross includes
class Thread;
class Network;
class MemoryManagerBase;
class MemoryManagerFast;
class PerformanceModel;
class ClockSkewMinimizationClient;
class ShmemPerfModel;
class TopologyInfo;
class CheetahManager;

#include "mem_component.h"
#include "fixed_types.h"
#include "lock.h"
#include "packet_type.h"
#include "subsecond_time.h"
#include "bbv_count.h"
#include "cpuid.h"
#include "hit_where.h"
#include <vector>
#include <map>
#include <queue>
#include <set>
#include "network.h"

struct MemoryResult {
   HitWhere::where_t hit_where;
   subsecond_time_t latency;
};

MemoryResult makeMemoryResult(HitWhere::where_t _hit_where, SubsecondTime _latency);
void applicationMemCopy(void *dest, const void *src, size_t n);
int const TLB_SHOOT_DOWN_SIZE=1;
void CoreNetworkCallback(void* obj, NetPacket packet);

class Core
{
   public:

      enum State
      {
         RUNNING = 0,
         INITIALIZING,
         STALLED,
         SLEEPING,
         WAKING_UP,
         IDLE,
         BROKEN,
         NUM_STATES
      };

      enum lock_signal_t
      {
         INVALID_LOCK_SIGNAL = 0,
         MIN_LOCK_SIGNAL,
         NONE = MIN_LOCK_SIGNAL,
         LOCK,
         UNLOCK,
         MAX_LOCK_SIGNAL = UNLOCK,
         NUM_LOCK_SIGNAL_TYPES = MAX_LOCK_SIGNAL - MIN_LOCK_SIGNAL + 1
      };

      enum mem_op_t
      {
         INVALID_MEM_OP = 0,
         MIN_MEM_OP,
         READ = MIN_MEM_OP,
         READ_EX,
         WRITE,
         MAX_MEM_OP = WRITE,
         NUM_MEM_OP_TYPES = MAX_MEM_OP - MIN_MEM_OP + 1
      };

      enum mem_origin_t
      {
         PAGE_TABLE_WALK = 0,
         NORMAL,
         NUM_MEM_ORIGINS
      };

      /* To what extend to make a memory access visible to the simulated instruction */
      enum MemModeled
      {
         MEM_MODELED_NONE,      /* Not at all (pure backdoor access) */
         MEM_MODELED_COUNT,     /* Count in #accesses/#misses */
         MEM_MODELED_COUNT_TLBTIME, /* Count in #accesses/#misses, queue TLBMissInstruction on TLB miss */
         MEM_MODELED_TIME,      /* Count + account for access latency (using MemAccessInstruction) */
         MEM_MODELED_FENCED,    /* Count + account for access latency as memory fence (using MemAccessInstruction) */
         MEM_MODELED_RETURN,    /* Count + time + return data to construct DynamicInstruction */
      };

      static const char * CoreStateString(State state);

      Core(SInt32 id);
      ~Core();

      // Query and update branch predictor, return true on mispredict
      bool accessBranchPredictor(IntPtr eip, bool taken, bool indirect, IntPtr target);

      MemoryResult readInstructionMemory(IntPtr address,
            UInt32 instruction_size);

      MemoryResult accessMemory(lock_signal_t lock_signal, mem_op_t mem_op_type, IntPtr d_addr, char* data_buffer, UInt32 data_size, MemModeled modeled = MEM_MODELED_NONE, IntPtr eip = 0, SubsecondTime now = SubsecondTime::MaxTime(), bool is_fault_mask = false);
      MemoryResult nativeMemOp(lock_signal_t lock_signal, mem_op_t mem_op_type, IntPtr d_addr, char* data_buffer, UInt32 data_size);

      void accessMemoryFast(bool icache, mem_op_t mem_op_type, IntPtr address);

      void logMemoryHit(bool icache, mem_op_t mem_op_type, IntPtr address, MemModeled modeled = MEM_MODELED_NONE, IntPtr eip = 0);
      bool countInstructions(IntPtr address, UInt32 count);

      void emulateCpuid(UInt32 eax, UInt32 ecx, cpuid_result_t &res) const;

      // network accessor since network is private
      int getId() const { return m_core_id; }
      Thread *getThread() const { return m_thread; }
      void setThread(Thread *thread) { m_thread = thread; }
      Network *getNetwork() { return m_network; }
      PerformanceModel *getPerformanceModel() { return m_performance_model; }
      ClockSkewMinimizationClient* getClockSkewMinimizationClient() const { return m_clock_skew_minimization_client; }
      MemoryManagerBase *getMemoryManager() { return m_memory_manager; }
      const MemoryManagerBase *getMemoryManager() const { return m_memory_manager; }
      ShmemPerfModel* getShmemPerfModel() { return m_shmem_perf_model; }
      const ComponentPeriod* getDvfsDomain() const { return m_dvfs_domain; }
      TopologyInfo* getTopologyInfo() { return m_topology_info; }
      const TopologyInfo* getTopologyInfo() const { return m_topology_info; }
      const CheetahManager* getCheetahManager() const { return m_cheetah_manager; }

      State getState() const { return m_core_state; }
      void setState(State core_state) { m_core_state = core_state; }
      UInt64 getInstructionCount() { return m_instructions; }
      BbvCount *getBbvCount() { return &m_bbv; }
      UInt64 getInstructionsCallback() { return m_instructions_callback; }
      bool isEnabledInstructionsCallback() { return m_instructions_callback != UINT64_MAX; }
      void setInstructionsCallback(UInt64 instructions) { m_instructions_callback = m_instructions + instructions; }
      void disableInstructionsCallback() { m_instructions_callback = UINT64_MAX; }

      void enablePerformanceModels();
      void disablePerformanceModels();

      // @RBERA: for measuring contribution of page-walk entries in cache
      void measureCacheStats();

      //Utr
      UInt64 *getUtrBitmap();

      void updateSpinCount(UInt64 instructions, SubsecondTime elapsed_time)
      {
         m_spin_loops++;
         m_spin_instructions += instructions;
         m_spin_elapsed_time += elapsed_time;
      }

      int CoreFlushTLB(int appid, IntPtr address);

      // TLB Shootdown Buffer
      struct TLBShootdownRequest {
         int app_id;
         bool requires_ack;
         core_id_t initiator_core_id;
         SubsecondTime timestamp;
         IntPtr id;  // use address of the first page to identify request
         std::array<IntPtr, TLB_SHOOT_DOWN_SIZE> addrs;
      };
      std::queue<TLBShootdownRequest> m_tlb_shootdown_buffer;
      Lock m_tlb_shootdown_buffer_lock;

      // 用于跟踪等待的 shootdown 响应
      struct PendingShootdown {
         IntPtr address{};
         std::set<core_id_t> pending_cores;  // 等待响应的核心集合
         std::set<bool> acked_pages; // 已确认刷新页面集合
         SubsecondTime max_end_time;
         Semaphore *sem;
         PendingShootdown(): sem(nullptr) {}
      };
      std::map<IntPtr, PendingShootdown> m_pending_shootdowns;
      Lock m_pending_shootdowns_lock;

      // Payload for the TLB Shootdown request
      struct TLBShootdownRequestPayload {
         int app_id;
         IntPtr request_id; // Unique ID (e.g., pages_array.front())
         std::array<IntPtr, TLB_SHOOT_DOWN_SIZE> addrs;
      };

      // Payload for the TLB Shootdown acknowledgment
      struct TLBShootdownAckPayload {
         IntPtr request_id;
         std::array<bool, TLB_SHOOT_DOWN_SIZE> flush_result;
         // (from_core_id and time are already metadata in NetPacket/ShmemMsg)
      };

      void initiateTLBShootdownBroadcast(TLBShootdownRequest &request);

      void enqueueTLBShootdownRequest(std::array<IntPtr, TLB_SHOOT_DOWN_SIZE> &pages_queue, core_id_t init_id, int app_id); //向 buffer 中添加 TLB shootdown 请求
      void processTLBShootdownBuffer(bool processing_remote_only); // 处理 buffer 中的 TLB shootdown 请求
      void handleRemoteTLBShootdownRequest(TLBShootdownRequest &request);
      void handleMsgFromOtherCore(core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg *shmem_msg);
      void networkHandleTLBShootdownRequest(PrL1PrL2DramDirectoryMSI::ShmemMsg *shmem_msg);
      void networkHandleTLBShootdownAck(PrL1PrL2DramDirectoryMSI::ShmemMsg *shmem_msg);

   private:
      core_id_t m_core_id;
      const ComponentPeriod* m_dvfs_domain;
      MemoryManagerBase *m_memory_manager;
      Thread *m_thread;
      Network *m_network;
      PerformanceModel *m_performance_model;
      ClockSkewMinimizationClient *m_clock_skew_minimization_client;
      Lock m_mem_lock;
      ShmemPerfModel* m_shmem_perf_model;
      BbvCount m_bbv;
      TopologyInfo *m_topology_info;
      CheetahManager *m_cheetah_manager;
      UInt64 *utr_bitmap;
      UInt64 number_of_shootdown_requests = 0;
      State m_core_state;
      ShmemPerf* m_shmem_perf;

      static Lock m_global_core_lock;

      MemoryResult initiateMemoryAccess(
            MemComponent::component_t mem_component,
            lock_signal_t lock_signal,
            mem_op_t mem_op_type,
            IntPtr address,
            Byte* data_buf, UInt32 data_size,
            MemModeled modeled,
            IntPtr eip,
            SubsecondTime now);

      void hookPeriodicInsCheck();
      void hookPeriodicInsCall();

      IntPtr m_icache_last_block;

      UInt64 m_spin_loops;
      UInt64 m_spin_instructions;
      SubsecondTime m_spin_elapsed_time;

      SubsecondTime tlb_flush_latency;
      SubsecondTime ipi_initiate_latency;
      SubsecondTime ipi_handle_latency;

   protected:
      // Optimized version of countInstruction has direct access to m_instructions and m_instructions_callback
      friend class InstructionModeling;

      // In contrast to core->m_performance_model->m_instructions, this one always increments,
      // also when performance modeling is disabled or when instrumenation mode is CACHE_ONLY or FAST_FORWARD
      UInt64 m_instructions;
      UInt64 m_instructions_callback;
      // HOOK_PERIODIC_INS implementation
      UInt64 m_instructions_hpi_callback;
      UInt64 m_instructions_hpi_last;
      static UInt64 g_instructions_hpi_global;
      static UInt64 g_instructions_hpi_global_callback;
};

#endif
