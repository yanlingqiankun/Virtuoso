// //
// // Created by shado on 25-6-12.
// //
// #include "baseline_allocator.h"
// #include "physical_memory_allocator.h"
// #include "simulator.h"
// #include "config.hpp"
// #include "hemem_allocator.h"
// #include <iostream>
// #include <queue>
//
// #include "mimicos.h"
//
// using namespace std;
//
// HememAllocator::HememAllocator(String name, UInt64 dram_size, UInt64 nvm_size, int max_order, int kernel_size, String frag_type)
//     :PhysicalMemoryAllocator(name, dram_size+nvm_size, kernel_size), dram_free_list("dram_free"), nvm_free_list("nvm_free")
// {
//
//     if (dram_size <= kernel_size) {
//         std::cout << "[Hemem] Kernel size larger than dram size, All program memory will be placed on nvm" << std::endl;
//         dram_size = 0;
//         nvm_size = nvm_size - (kernel_size - dram_size);
//     } else {
//         dram_size -= kernel_size;
//     }
//     // In config files, dram_size and nvm_size in MB
//     dram_size = dram_size << 20;
//     nvm_size = nvm_size << 20;
//     kernel_size = kernel_size << 20;
//     std::cout << "[Hemem] Creating Hemem Paging Allocator" << std::endl;
//
//     for (int i = 0; i < dram_size/page_size; i++) {
//         Hemem::hemem_page *p = new Hemem::hemem_page();
//         p->phy_addr = i * page_size + kernel_size;
//         p->present = false;
//         p->in_dram = true;
//         p->pt = Hemem::pagesize_to_pt(page_size);
//         Hemem::enqueue(&dram_free_list, p);
//     }
//
//     for (int i = 0; i < nvm_size/page_size; i++) {
//         Hemem::hemem_page *p = new Hemem::hemem_page();
//         p->phy_addr = dram_size+kernel_size+ i * page_size;
//         p->present = false;
//         p->in_dram = false;
//         p->pt = Hemem::pagesize_to_pt(page_size);
//         Hemem::enqueue(&nvm_free_list, p);
//     }
//
//     std::cout << "[Hemem] pages in DRAM : " << dram_free_list.numentries << std::endl;
//     std::cout << "[Hemem] pages in NVM : " << nvm_free_list.numentries << std::endl;
//     this->dram_reserved_threshold = dram_free_list.numentries / 10;
//     std::cout << "[Hemem] Reserved DRAM pages for migration: " << this->dram_reserved_threshold << std::endl;
// }
//
// HememAllocator::~HememAllocator() {
//
// }
//
// std::pair<UInt64,UInt64> HememAllocator::allocate(UInt64 bytes, UInt64 address, UInt64 core_id, bool is_pagetable_allocation) {
//     // std::cout << "0x" << std::hex << &dram_free_list << std::endl;
//     if (is_pagetable_allocation) {
//         UInt64 physical_page = handle_page_table_allocations(bytes);
//         return make_pair(physical_page, 12); // Return the physical address and the page size
//     } else {
//         // std::cout << "[BUDDY] page addr = 0x" << std::hex << (address&(~((1<<12)-1))) << " addr = 0x" << address << std::endl;
//         mutex_alloc.lock();
//         Hemem::hemem_page *page = nullptr;
//         if (dram_free_list.numentries > this->dram_reserved_threshold) {
//             page = Hemem::dequeue(&dram_free_list);
//         }
//         if (page != nullptr) {
//             assert(!page->present);
//             page->present = true;
//         } else {
//             page = Hemem::dequeue(&nvm_free_list);
//             if (page == nullptr) {
//                 std::cerr << "[Hemem] OUT OF MEMORY!!!!" << std::endl;
//             }
//             assert(page != nullptr);
//             assert(!page->present);
//             page->present = true;
//         }
//         mutex_alloc.unlock();
//         page->vaddr = address & BASE_PAGE_MASK;
//         Sim()->getMimicOS()->getPageMigrationHandler()->page_fault(address & BASE_PAGE_MASK, page);
//         return make_pair(page->phy_addr, 12);
//     }
// }
//
// Hemem::hemem_page *HememAllocator::getAFreePage(bool is_dram) {
//     Hemem::hemem_page *ret = nullptr;
//     mutex_alloc.lock();
//     if (is_dram) {
//         ret = Hemem::dequeue(&dram_free_list);
//     } else {
//         ret = Hemem::dequeue(&nvm_free_list);
//     }
//     mutex_alloc.unlock();
//     return ret;
// }
//
// std::queue<Hemem::hemem_page*> HememAllocator::getFreePages(std::queue<bool> is_dram) {
//     std::queue<Hemem::hemem_page*> ret;
//     mutex_alloc.lock();
//     while (!is_dram.empty()) {
//         bool dram = is_dram.front();
//         is_dram.pop();
//         Hemem::hemem_page *page = nullptr;
//         if (dram) {
//             page = Hemem::dequeue(&dram_free_list);
//         } else {
//             page = Hemem::dequeue(&nvm_free_list);
//         }
//         if (page == nullptr) {
//             // Free list empty, stop here
//             break;
//         }
//         ret.push(page);
//     }
//     mutex_alloc.unlock();
//     return ret;
// }
//
// void HememAllocator::deallocate(UInt64 region_begin, UInt64 core_id)
// {
// }
//
// void HememAllocator::deallocate(Hemem::hemem_page *page, bool is_dram, UInt64 core_id) {
//     if (is_dram) {
//         Hemem::enqueue(&dram_free_list, page);
//     } else {
//         Hemem::enqueue(&nvm_free_list, page);
//     }
// }
//
// void HememAllocator::deallocatePages(std::queue<Hemem::hemem_page*> pages, std::queue<bool> is_dram, UInt64 app_id) {
//     mutex_alloc.lock();
//     while (!pages.empty() && !is_dram.empty()) {
//         Hemem::hemem_page *page = pages.front();
//         pages.pop();
//         bool dram = is_dram.front();
//         is_dram.pop();
//         if (!dram) {
//             Hemem::enqueue(&dram_free_list, page);
//         } else {
//             Hemem::enqueue(&nvm_free_list, page);
//         }
//     }
//     mutex_alloc.unlock();
//
// }
//
// std::vector<Range> HememAllocator::allocate_ranges(IntPtr start_va, IntPtr end_va, int app_id)
// {
//     // Not implemented - just return an empty vector
//     std::vector<Range> ranges;
//     return ranges;
// }
//
// void HememAllocator::fragment_memory()
// {
//     return;
// }

#include "hemem_allocator.h"
#include "simulator.h"
#include "config.hpp"
#include "mimicos.h"
#include <iostream>

using namespace std;

static const UInt64 PAGE_SIZE = 4096;

HememAllocator::HememAllocator(String name, UInt64 dram_size, UInt64 nvm_size, int max_order, int kernel_size, String frag_type)
    : PhysicalMemoryAllocator(name, dram_size + nvm_size, kernel_size)
{
    if (dram_size <= kernel_size) {
        std::cout << "[Hemem] Kernel size larger than dram size..." << std::endl;
        dram_size = 0;
        nvm_size = nvm_size - (kernel_size - dram_size);
    }

    std::cout << "[Hemem] Creating Buddy Allocators (Lazy Page Construction)" << std::endl;

    dram_buddy = new Buddy(dram_size, max_order, kernel_size, frag_type);
    nvm_buddy = new Buddy(nvm_size, max_order, 0, frag_type);

    m_dram_size_bytes = dram_size * 1024 * 1024;
    m_nvm_size_bytes = nvm_size * 1024 * 1024;

    this->dram_reserved_threshold = (dram_buddy->getTotalPages()) / 10;
    this->m_preferred_node = Sim()->getCfg()->getInt("perf_model/hemem_allocator/preferred_node");

    std::cout << "[Hemem] Allocator Initialized with preferred mem node "<< this->m_preferred_node <<". Metadata will be created on-demand." << std::endl;
}

HememAllocator::~HememAllocator() {
    delete dram_buddy;
    delete nvm_buddy;
    for (auto& pair : m_active_pages) {
        delete pair.second;
    }
    m_active_pages.clear();
}

Hemem::hemem_page* HememAllocator::create_active_page(UInt64 phy_addr, bool is_dram) {
    auto it = m_active_pages.find(phy_addr);
    if (it != m_active_pages.end()) {
        Hemem::hemem_page* p = it->second;

        p->present = true;
        p->in_dram = is_dram;
        p->naccesses = 0;
        p->migrating = false;

        return p;
    }

    Hemem::hemem_page *p = new Hemem::hemem_page();
    p->phy_addr = phy_addr;
    p->present = true;
    p->in_dram = is_dram;
    p->naccesses = 0;
    p->migrating = false;
    p->pt = Hemem::pagesize_to_pt(PAGE_SIZE);

    m_active_pages[phy_addr] = p;
    return p;
}

void HememAllocator::destroy_active_page(UInt64 phy_addr) {
    auto it = m_active_pages.find(phy_addr);
    if (it != m_active_pages.end()) {
        delete it->second;
        m_active_pages.erase(it);
    }
}

std::pair<UInt64, UInt64> HememAllocator::allocate(UInt64 bytes, UInt64 address, UInt64 core_id, bool is_pagetable_allocation) {
    if (is_pagetable_allocation) {
        UInt64 physical_page = handle_page_table_allocations(bytes);
        return make_pair(physical_page, 12);
    }
    else {
        mutex_alloc.lock();

        UInt64 allocated_phy_addr = -1;
        bool is_in_dram = false;
        if (m_preferred_node == 0) {
            // --- preferred node == 0 ---
            if (dram_buddy->getFreePages() > this->dram_reserved_threshold) {
                allocated_phy_addr = dram_buddy->allocate(bytes, 0, core_id);
                if (allocated_phy_addr != static_cast<UInt64>(-1)) {
                    is_in_dram = true;
                }
            }
            // DRAM is full
            if (allocated_phy_addr == static_cast<UInt64>(-1)) {
                allocated_phy_addr = nvm_buddy->allocate(bytes, 0, core_id);
                if (allocated_phy_addr != static_cast<UInt64>(-1)) {
                    allocated_phy_addr += m_dram_size_bytes;
                    is_in_dram = false;
                }
            }
        }
        else {
            // --- preferred node == 1 ---
            allocated_phy_addr = nvm_buddy->allocate(bytes, 0, core_id);
            if (allocated_phy_addr != static_cast<UInt64>(-1)) {
                allocated_phy_addr += m_dram_size_bytes;
                is_in_dram = false;
            }
            // NVM is full
            else if (dram_buddy->getFreePages() > this->dram_reserved_threshold) {
                allocated_phy_addr = dram_buddy->allocate(bytes, 0, core_id);
                if (allocated_phy_addr != static_cast<UInt64>(-1)) {
                    is_in_dram = true;
                }
            }
        }

        if (allocated_phy_addr == static_cast<UInt64>(-1)) {
            std::cerr << "[Hemem] OUT OF MEMORY!!!! (NVM first, DRAM fallback failed)" << std::endl;
            mutex_alloc.unlock();
            assert(false);
            return make_pair(-1, 0);
        }

        Hemem::hemem_page *page = create_active_page(allocated_phy_addr, is_in_dram);

        mutex_alloc.unlock();

        page->vaddr = address & BASE_PAGE_MASK;
        Sim()->getMimicOS()->getPageMigrationHandler()->page_fault(address & BASE_PAGE_MASK, page);

        return make_pair(allocated_phy_addr, 12);
    }
}

Hemem::hemem_page *HememAllocator::getAFreePage(bool is_dram) {
    mutex_alloc.lock();
    UInt64 phy_addr = -1;

    if (is_dram) {
        phy_addr = dram_buddy->allocate(PAGE_SIZE, 0, 0);
    } else {
        phy_addr = nvm_buddy->allocate(PAGE_SIZE, 0, 0);
        if (phy_addr != static_cast<UInt64>(-1)) {
            phy_addr += m_dram_size_bytes;
        }
    }

    if (phy_addr == static_cast<UInt64>(-1)) {
        mutex_alloc.unlock();
        return nullptr;
    }

    Hemem::hemem_page *page = create_active_page(phy_addr, is_dram);

    mutex_alloc.unlock();
    return page;
}

std::queue<Hemem::hemem_page*> HememAllocator::getFreePages(std::queue<bool> is_dram_queue) {
    std::queue<Hemem::hemem_page*> ret;
    mutex_alloc.lock();

    std::queue<bool> temp_queue = is_dram_queue;

    while (!temp_queue.empty()) {
        bool is_dram = temp_queue.front();
        temp_queue.pop();

        UInt64 phy_addr = -1;
        if (is_dram) {
            phy_addr = dram_buddy->allocate(PAGE_SIZE, 0, 0);
        } else {
            phy_addr = nvm_buddy->allocate(PAGE_SIZE, 0, 0);
            if (phy_addr != static_cast<UInt64>(-1)) {
                phy_addr += m_dram_size_bytes;
            }
        }

        if (phy_addr == static_cast<UInt64>(-1)) break;

        // 按需创建
        ret.push(create_active_page(phy_addr, is_dram));
    }

    mutex_alloc.unlock();
    return ret;
}

void HememAllocator::deallocate(UInt64 region_begin, UInt64 core_id)
{
    UInt64 region_end = region_begin + PAGE_SIZE - 1;

    mutex_alloc.lock();

    if (region_begin < m_dram_size_bytes) {
        dram_buddy->free(region_begin, region_end);
    } else {
        if (region_begin >= m_dram_size_bytes) {
            nvm_buddy->free(region_begin - m_dram_size_bytes, region_end - m_dram_size_bytes);
        }
    }

    destroy_active_page(region_begin);

    mutex_alloc.unlock();
}

void HememAllocator::deallocate(Hemem::hemem_page *page, bool is_dram, UInt64 core_id) {
    if (!page) return;

    UInt64 start_addr = page->phy_addr;
    UInt64 end_addr = start_addr + PAGE_SIZE - 1;

    mutex_alloc.lock();

    if (is_dram) {
        if (start_addr < m_dram_size_bytes) {
            dram_buddy->free(start_addr, end_addr);
        }
    } else {
        if (start_addr >= m_dram_size_bytes) {
            nvm_buddy->free(start_addr - m_dram_size_bytes, end_addr - m_dram_size_bytes);
        }
    }

    page->present = false;
    page->naccesses = 0;
    page->migrating = false;

    mutex_alloc.unlock();
}

void HememAllocator::deallocatePages(std::queue<Hemem::hemem_page*> pages, std::queue<bool> is_dram_queue, UInt64 app_id) {
    mutex_alloc.lock();
    while (!pages.empty() && !is_dram_queue.empty()) {
        Hemem::hemem_page *page = pages.front();
        pages.pop();
        bool is_dram = is_dram_queue.front();
        is_dram_queue.pop();

        UInt64 start_addr = page->phy_addr;
        UInt64 end_addr = start_addr + PAGE_SIZE - 1;

        if (is_dram) {
             if (start_addr < m_dram_size_bytes)
                dram_buddy->free(start_addr, end_addr);
        } else {
             if (start_addr >= m_dram_size_bytes)
                nvm_buddy->free(start_addr - m_dram_size_bytes, end_addr - m_dram_size_bytes);
        }

        page->present = false;
        page->naccesses = 0;
        page->migrating = false;
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