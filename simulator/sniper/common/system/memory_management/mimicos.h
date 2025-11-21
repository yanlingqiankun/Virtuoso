
#pragma once

#include "buddy_allocator.h"
#include "physical_memory_allocator.h"
#include "utopia.h"
#include "page_fault_handler.h"
#include "pagetable.h"
#include "rangetable.h"
#include "subsecond_time.h"
#include <unordered_map>

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

    PageTracer *page_tracer;
    PageMigration *page_migration_handler;
    ComponentLatency tlb_flush_latency;
    ComponentLatency ipi_initiate_latency;
    ComponentLatency ipi_handle_latency;

    map<IntPtr,pair<array<IntPtr, TLB_SHOOT_DOWN_SIZE>,array<IntPtr, TLB_SHOOT_DOWN_SIZE>>> DMA_map; // request_id -> <virtual addr, new physical addr>

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

    PageTracer *getPageTracer() {return page_tracer;}
    PageMigration *getPageMigrationHandler() { return page_migration_handler; }
    SubsecondTime getTLBFlushLatency() {return tlb_flush_latency.getLatency(); }
    core_id_t flushTLB(int app_id, array<IntPtr, TLB_SHOOT_DOWN_SIZE> addrs);
    bool move_pages(std::queue<Hemem::hemem_page*> pages, std::queue<bool> migrate_up, int app_id);
    void DMA_migrate(IntPtr move_id, subsecond_time_t finish_time, int app_id = 0);
};