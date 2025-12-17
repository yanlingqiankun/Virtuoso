//
// Created by shado on 25-6-12.
//

#ifndef HEMEM_ALLOCATOR_H
#define HEMEM_ALLOCATOR_H
#include "hemem.h"
#include "fixed_types.h"
#include "vma.h"
#include "fixed_types.h"
#include <mutex>
#include <queue>
#include <unordered_map>
#include "buddy_allocator.h"
#include "physical_memory_allocator.h"

#include "../page_migration/hemem.h"

class HememAllocator :public PhysicalMemoryAllocator {
public:
    HememAllocator(String name, UInt64 dram_size, UInt64 nvm_size, int max_order, int kernel_size, String frag_type);
    ~HememAllocator();

    std::pair<UInt64,UInt64> allocate(UInt64 size, UInt64 address = 0, UInt64 core_id = -1, bool is_pagetable_allocation = false);
    std::vector<Range> allocate_ranges(IntPtr start_va, IntPtr end_va, int app_id);
    void deallocate(UInt64 address, UInt64 core_id);
    void fragment_memory();

    void deallocate(Hemem::hemem_page *page, bool is_dram, UInt64 core_id);
    Hemem::hemem_page *getAFreePage(bool is_dram);
    std::queue<Hemem::hemem_page*> getFreePages(std::queue<bool> is_dram);
    void deallocatePages(std::queue<Hemem::hemem_page*> pages, std::queue<bool> is_dram, UInt64 app_id);

private:
    Buddy *dram_buddy;
    Buddy *nvm_buddy;
    std::unordered_map<UInt64, Hemem::hemem_page*> m_active_pages;
    // Hemem::fifo_list dram_free_list;
    // Hemem::fifo_list nvm_free_list;
    int page_size = 4096;
    std::mutex mutex_alloc;
    UInt64 dram_reserved_threshold;
    UInt64 m_dram_size_bytes;
    UInt64 m_nvm_size_bytes;
    UInt64 m_kernel_size_bytes;

    Hemem::hemem_page* create_active_page(UInt64 phy_addr, bool is_dram);
    void destroy_active_page(UInt64 phy_addr);
};

#endif //HEMEM_ALLOCATOR_H
