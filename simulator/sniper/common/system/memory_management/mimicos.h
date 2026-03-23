
#pragma once

#include "buddy_allocator.h"
#include "physical_memory_allocator.h"
#include "utopia.h"
#include "page_fault_handler.h"
#include "pagetable.h"
#include "rangetable.h"
#include "subsecond_time.h"
#include "stats.h"
#include <unordered_map>
#include <mutex>
#include <atomic>

#include "page_migration/memtis.h"

using namespace std;

class MimicOS
{
private:
    // Structure to simulate per-core interrupt state for TLB shootdown
    struct CoreInterruptState {
        bool pending_tlb_flush;
        UInt64 flush_vaddr;
        int flush_app_id;
    };

private:

    PhysicalMemoryAllocator *m_memory_allocator; // This is the physical memory allocator
    bool is_guest;
    String mimicos_name;
    PageFaultHandlerBase *page_fault_handler;
    bool vmas_provided;
    

    String page_table_type;
    String page_table_name;
    ComponentLatency m_page_fault_latency;

    String range_table_type;
    String range_table_name;

    
    int number_of_page_sizes;
    int *page_size_list;

    // Use an unordered map to store the page table for each application
    std::unordered_map<UInt64, ParametricDramDirectoryMSI::PageTable*> page_tables;
    std::unordered_map<UInt64, ParametricDramDirectoryMSI::RangeTable*> range_tables;

    // Use an unordered map to store the virtual memory areas for each application
    std::unordered_map<UInt64, std::vector<VMA>> vm_areas;

    PageMigration *page_migration_handler;
    ComponentLatency tlb_flush_latency;
    ComponentLatency ipi_initiate_latency;
    ComponentLatency ipi_handle_latency;

    map<IntPtr,pair<array<IntPtr, TLB_SHOOT_DOWN_MAX_SIZE>,array<IntPtr, TLB_SHOOT_DOWN_MAX_SIZE>>> DMA_map; // request_id -> <virtual addr, new physical addr>
    std::mutex m_dma_map_lock;

    bool one_app;
    bool m_site_enabled;
    bool m_nomad_enabled;
    UInt64 m_nomad_copy_latency_us;
    std::vector<SubsecondTime> m_dma_copy_latencies; // Indexed by page count (1-based)

    std::atomic<uint32_t> m_rr_issuer_counter{0}; // Round-robin counter for TLB shootdown issuer selection

    // NOMAD: internal TPM + fast demotion implementation
    bool move_pages_nomad(std::queue<Hemem::hemem_page*> pages, std::queue<bool> migrate_up, int app_id);
    // Original blocking migration (used by move_pages when NOMAD disabled, and as fallback for dirty demotions)
    bool move_pages_traditional(std::queue<Hemem::hemem_page*> pages, std::queue<bool> migrate_up, int app_id);


    // Page migration statistics
    struct MigrationStats
    {
        UInt64 move_pages_calls;             // Number of move_pages() invocations
        UInt64 move_pages_syscall_calls;     // Number of move_pages_syscall() invocations
        UInt64 total_migrations_requested;   // Total pages submitted for migration
        UInt64 pages_migrated_to_dram;       // Pages successfully migrated NVM -> DRAM
        UInt64 pages_migrated_to_nvm;        // Pages successfully migrated DRAM -> NVM
        UInt64 migration_skipped_same_tier;  // Skipped: page already in target tier
        UInt64 migration_skipped_invalid;    // Skipped: null or invalid source page
        UInt64 migration_failed_no_free;     // Failed: no free page in destination tier
        UInt64 tlb_shootdown_batches;        // Number of TLB shootdown batches issued
        UInt64 dma_migrations_completed;     // DMA migration completions (PTE re-validated)
        UInt64 syscall_lookup_failed;        // Pages not found during syscall vaddr translation
        UInt64 site_shootdowns_avoided;      // SITE: Shootdowns avoided due to expired TLB entries
        UInt64 site_shootdowns_performed;    // SITE: Shootdowns still required (entries not yet expired)
        UInt64 beneficial_dram_access_samples;  // Access samples that hit a page migrated to DRAM (Benefit = samples * frequency * latency_diff)
        UInt64 penalized_nvm_access_samples;    // Access samples that hit a page migrated down to NVM (Penalty = samples * frequency * latency_diff)
        // NOMAD statistics
        UInt64 nomad_tpm_attempts;
        UInt64 nomad_tpm_commits;
        UInt64 nomad_tpm_aborts;
        UInt64 nomad_fast_demotions;
        UInt64 nomad_traditional_demotions;
        UInt64 nomad_shadow_faults;
    } migration_stats;

public:
    MimicOS(bool _is_guest);
    ~MimicOS();

    void handle_page_fault(IntPtr address, IntPtr core_id, int frames);
    void createApplication(int app_id);

    String getName() { return mimicos_name; }

    PhysicalMemoryAllocator *getMemoryAllocator() { return m_memory_allocator; }

    ParametricDramDirectoryMSI::PageTable* getPageTable(int app_id) { return page_tables[app_id]; }
    ParametricDramDirectoryMSI::RangeTable* getRangeTable(int app_id) { return range_tables[app_id]; }

    std::vector<VMA> getVMA(int app_id) { return vm_areas[app_id]; }

    void setPageTableType(String type) { page_table_type = type; }
    void setPageTableName(String name) { page_table_name = name; }
    String getPageTableType() { return page_table_type; }
    String getPageTableName() { return page_table_name; }

    void setRangeTableType(String type) { range_table_type = type; }
    void setRangeTableName(String name) { range_table_name = name; }

    String getRangeTableType() { return range_table_type; }
    String getRangeTableName() { return range_table_name; }

    int getNumberOfPageSizes() { return number_of_page_sizes; }
    int* getPageSizeList() { return page_size_list; }

    PageFaultHandlerBase *getPageFaultHandler() { return page_fault_handler; }
    SubsecondTime getPageFaultLatency() { return m_page_fault_latency.getLatency(); }

    PageMigration *getPageMigrationHandler() { return page_migration_handler; }
    SubsecondTime getTLBFlushLatency() {return tlb_flush_latency.getLatency(); }
    core_id_t flushTLB(int app_id, array<IntPtr, TLB_SHOOT_DOWN_MAX_SIZE> addrs, array<IntPtr, TLB_SHOOT_DOWN_MAX_SIZE> phy_addrs, int page_num);
    bool move_pages(std::queue<Hemem::hemem_page*> pages, std::queue<bool> migrate_up, int app_id);
    void DMA_migrate(IntPtr move_id, subsecond_time_t finish_time, int app_id = 0);
    bool move_pages_syscall(std::queue<IntPtr> src_pages_address_queue, std::queue<bool> migrate_up_queue, int app_id);
    bool is_multi_threaded(){return one_app;}
    
    void incrementBeneficialDramAccessSamples() { migration_stats.beneficial_dram_access_samples++; }
    void incrementPenalizedNvmAccessSamples() { migration_stats.penalized_nvm_access_samples++; }
};