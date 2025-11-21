//
// Created by shado on 25-6-12.
//
#include "baseline_allocator.h"
#include "physical_memory_allocator.h"
#include "simulator.h"
#include "config.hpp"
#include "hemem_allocator.h"
#include <iostream>
#include <queue>

#include "mimicos.h"

using namespace std;

HememAllocator::HememAllocator(String name, UInt64 dram_size, UInt64 nvm_size, int max_order, int kernel_size, String frag_type)
    :PhysicalMemoryAllocator(name, dram_size+nvm_size, kernel_size), dram_free_list("dram_free"), nvm_free_list("nvm_free")
{

    if (dram_size <= kernel_size) {
        std::cout << "[Hemem] Kernel size larger than dram size, All program memory will be placed on nvm" << std::endl;
        dram_size = 0;
        nvm_size = nvm_size - (kernel_size - dram_size);
    } else {
        dram_size -= kernel_size;
    }
    // In config files, dram_size and nvm_size in MB
    dram_size = dram_size << 20;
    nvm_size = nvm_size << 20;
    std::cout << "[Hemem] Creating Hemem Paging Allocator" << std::endl;

    for (int i = 0; i < dram_size/page_size; i++) {
        Hemem::hemem_page *p = new Hemem::hemem_page();
        p->phy_addr = i * page_size;
        p->present = false;
        p->in_dram = true;
        p->pt = Hemem::pagesize_to_pt(page_size);
        Hemem::enqueue(&dram_free_list, p);
    }

    for (int i = 0; i < nvm_size/page_size; i++) {
        Hemem::hemem_page *p = new Hemem::hemem_page();
        p->phy_addr = dram_size+ i * page_size;
        p->present = false;
        p->in_dram = false;
        p->pt = Hemem::pagesize_to_pt(page_size);
        Hemem::enqueue(&nvm_free_list, p);
    }

    std::cout << "[Hemem] pages in DRAM : " << dram_free_list.numentries << std::endl;
    std::cout << "[Hemem] pages in NVM : " << nvm_free_list.numentries << std::endl;
    this->dram_reserved_threshold = dram_free_list.numentries / 10;
    std::cout << "[Hemem] Reserved DRAM pages for migration: " << this->dram_reserved_threshold << std::endl;
}

HememAllocator::~HememAllocator() {

}

std::pair<UInt64,UInt64> HememAllocator::allocate(UInt64 bytes, UInt64 address, UInt64 core_id, bool is_pagetable_allocation) {
    // std::cout << "0x" << std::hex << &dram_free_list << std::endl;
    if (is_pagetable_allocation) {
        UInt64 physical_page = handle_page_table_allocations(bytes);
        return make_pair(physical_page, 12); // Return the physical address and the page size
    } else {
        // std::cout << "[BUDDY] page addr = 0x" << std::hex << (address&(~((1<<12)-1))) << " addr = 0x" << address << std::endl;
        mutex_alloc.lock();
        Hemem::hemem_page *page = nullptr;
        if (dram_free_list.numentries > this->dram_reserved_threshold) {
            page = Hemem::dequeue(&dram_free_list);
        }
        if (page != nullptr) {
            assert(!page->present);
            page->present = true;
        } else {
            page = Hemem::dequeue(&nvm_free_list);
            if (page == nullptr) {
                std::cerr << "[Hemem] OUT OF MEMORY!!!!" << std::endl;
            }
            assert(page != nullptr);
            assert(!page->present);
            page->present = true;
        }
        mutex_alloc.unlock();
        page->vaddr = address & BASE_PAGE_MASK;
        Sim()->getMimicOS()->getPageMigrationHandler()->page_fault(address & BASE_PAGE_MASK, page);
        return make_pair(page->phy_addr, 12);
    }
}

Hemem::hemem_page *HememAllocator::getAFreePage(bool is_dram) {
    Hemem::hemem_page *ret = nullptr;
    mutex_alloc.lock();
    if (is_dram) {
        ret = Hemem::dequeue(&dram_free_list);
    } else {
        ret = Hemem::dequeue(&nvm_free_list);
    }
    mutex_alloc.unlock();
    return ret;
}

std::queue<Hemem::hemem_page*> HememAllocator::getFreePages(std::queue<bool> is_dram) {
    std::queue<Hemem::hemem_page*> ret;
    mutex_alloc.lock();
    while (!is_dram.empty()) {
        bool dram = is_dram.front();
        is_dram.pop();
        Hemem::hemem_page *page = nullptr;
        if (dram) {
            page = Hemem::dequeue(&dram_free_list);
        } else {
            page = Hemem::dequeue(&nvm_free_list);
        }
        if (page == nullptr) {
            // Free list empty, stop here
            break;
        }
        ret.push(page);
    }
    mutex_alloc.unlock();
    return ret;
}

void HememAllocator::deallocate(UInt64 region_begin, UInt64 core_id)
{
}

void HememAllocator::deallocate(Hemem::hemem_page *page, bool is_dram, UInt64 core_id) {
    if (is_dram) {
        Hemem::enqueue(&dram_free_list, page);
    } else {
        Hemem::enqueue(&nvm_free_list, page);
    }
}

void HememAllocator::deallocatePages(std::queue<Hemem::hemem_page*> pages, std::queue<bool> is_dram, UInt64 app_id) {
    mutex_alloc.lock();
    while (!pages.empty() && !is_dram.empty()) {
        Hemem::hemem_page *page = pages.front();
        pages.pop();
        bool dram = is_dram.front();
        is_dram.pop();
        if (!dram) {
            Hemem::enqueue(&dram_free_list, page);
        } else {
            Hemem::enqueue(&nvm_free_list, page);
        }
    }
    mutex_alloc.unlock();

}

std::vector<Range> HememAllocator::allocate_ranges(IntPtr start_va, IntPtr end_va, int app_id)
{
    // Not implemented - just return an empty vector
    std::vector<Range> ranges;
    return ranges;
}

void HememAllocator::fragment_memory()
{
    return;
}