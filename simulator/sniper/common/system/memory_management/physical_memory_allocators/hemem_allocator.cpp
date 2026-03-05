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
    // Buddy allocator operates in page numbers (PAGE_SIZE = 4096 bytes)
    // m_dram_size_pages is used to offset NVM page numbers to avoid overlap
    m_dram_size_pages = m_dram_size_bytes / PAGE_SIZE;

    // Only reserve DRAM for migration if migration is enabled
    if (Sim()->getCfg()->hasKey("migration/migration_enable") && Sim()->getCfg()->getBool("migration/migration_enable")) {
        this->dram_reserved_threshold = (dram_buddy->getTotalPages()) / 10;
        std::cout << "[Hemem] Migration enabled: reserving " << this->dram_reserved_threshold
                  << " DRAM pages for migration" << std::endl;
    } else {
        this->dram_reserved_threshold = 0;
        std::cout << "[Hemem] Migration disabled: no DRAM pages reserved" << std::endl;
    }
    this->m_preferred_node = Sim()->getCfg()->getInt("perf_model/hemem_allocator/preferred_node");

    std::cout << "[Hemem] Allocator Initialized with preferred mem node "<< this->m_preferred_node <<". Metadata will be created on-demand." << std::endl;

    // --- Initialize allocation statistics ---
    bzero(&alloc_stats, sizeof(alloc_stats));
    registerStatsMetric(name, 0, "alloc_dram_pages", &alloc_stats.alloc_dram_pages);
    registerStatsMetric(name, 0, "alloc_nvm_pages", &alloc_stats.alloc_nvm_pages);
    registerStatsMetric(name, 0, "migration_alloc_dram", &alloc_stats.migration_alloc_dram);
    registerStatsMetric(name, 0, "migration_alloc_nvm", &alloc_stats.migration_alloc_nvm);
    registerStatsMetric(name, 0, "dealloc_dram_pages", &alloc_stats.dealloc_dram_pages);
    registerStatsMetric(name, 0, "dealloc_nvm_pages", &alloc_stats.dealloc_nvm_pages);
    registerStatsMetric(name, 0, "alloc_failed_oom", &alloc_stats.alloc_failed_oom);
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

        // Safety: if the page's phy_addr doesn't match the key, it means
        // this entry is stale (migration swapped the phy_addr).
        // Remove the stale entry and create a fresh page.
        if (p->phy_addr != phy_addr) {
            // Don't delete p — it may still be in use by the migration handler
            // under its new phy_addr. Just remove the stale mapping.
            m_active_pages.erase(it);
        } else {
            p->present = true;
            p->in_dram = is_dram;
            p->naccesses = 0;
            p->migrating = false;
            return p;
        }
    }

    Hemem::hemem_page *p = new Hemem::hemem_page();
    p->phy_addr = phy_addr;
    p->present = true;
    p->in_dram = is_dram;
    p->initial_in_dram = is_dram;
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

        UInt64 allocated_phy_addr = static_cast<UInt64>(-1);
        bool is_in_dram = false;
        // NOTE: buddy->allocate() returns a PAGE NUMBER (page index), not a byte address.
        // We must multiply by PAGE_SIZE to obtain the physical byte address.
        if (m_preferred_node == 0) {
            // --- preferred node == 0 (DRAM first) ---
            if (dram_buddy->getFreePages() > this->dram_reserved_threshold) {
                UInt64 page_num = dram_buddy->allocate(bytes, 0, core_id);
                if (page_num != static_cast<UInt64>(-1)) {
                    // Convert page number -> byte physical address
                    allocated_phy_addr = page_num * PAGE_SIZE;
                    is_in_dram = true;
                }
            }
            // DRAM is full, fall back to NVM
            if (allocated_phy_addr == static_cast<UInt64>(-1)) {
                UInt64 page_num = nvm_buddy->allocate(bytes, 0, core_id);
                if (page_num != static_cast<UInt64>(-1)) {
                    // NVM page number -> byte address: NVM starts after DRAM in physical space
                    allocated_phy_addr = page_num * PAGE_SIZE + m_dram_size_bytes;
                    is_in_dram = false;
                }
            }
        }
        else {
            // --- preferred node == 1 (NVM first) ---
            UInt64 page_num = nvm_buddy->allocate(bytes, 0, core_id);
            if (page_num != static_cast<UInt64>(-1)) {
                allocated_phy_addr = page_num * PAGE_SIZE + m_dram_size_bytes;
                is_in_dram = false;
            }
            // NVM is full, fall back to DRAM
            else if (dram_buddy->getFreePages() > this->dram_reserved_threshold) {
                page_num = dram_buddy->allocate(bytes, 0, core_id);
                if (page_num != static_cast<UInt64>(-1)) {
                    allocated_phy_addr = page_num * PAGE_SIZE;
                    is_in_dram = true;
                }
            }
        }

        if (allocated_phy_addr == static_cast<UInt64>(-1)) {
            std::cerr << "[Hemem] OUT OF MEMORY!!!! (NVM first, DRAM fallback failed)" << std::endl;
            alloc_stats.alloc_failed_oom++;
            mutex_alloc.unlock();
            assert(false);
            return make_pair(-1, 0);
        }

        Hemem::hemem_page *page = create_active_page(allocated_phy_addr, is_in_dram);

        mutex_alloc.unlock();

        page->vaddr = address & BASE_PAGE_MASK;
        Sim()->getMimicOS()->getPageMigrationHandler()->page_fault(address & BASE_PAGE_MASK, page);

        // --- Allocation stats ---
        if (is_in_dram) {
            alloc_stats.alloc_dram_pages++;
        } else {
            alloc_stats.alloc_nvm_pages++;
        }

                // --- Real-time Memory Monitoring ---
        // Print stats every 256 allocations (every 1MB)
        if ((alloc_stats.alloc_dram_pages + alloc_stats.alloc_nvm_pages) % 256 == 0) {
            UInt64 total_dram_used = alloc_stats.alloc_dram_pages * 4096; // bytes
            UInt64 total_nvm_used = alloc_stats.alloc_nvm_pages * 4096;   // bytes
            
            std::cout << "[Hemem Monitor] "
                      << "DRAM Usage: " << (total_dram_used / 1024 / 1024) << " MB (" 
                      << alloc_stats.alloc_dram_pages << " pages) | "
                      << "NVM Usage: " << (total_nvm_used / 1024 / 1024) << " MB (" 
                      << alloc_stats.alloc_nvm_pages << " pages) | "
                      << "DRAM Free: " << dram_buddy->getFreePages() << " pages"
                      << std::endl;
        }

        return make_pair(allocated_phy_addr, 12);
    }
}

Hemem::hemem_page *HememAllocator::getAFreePage(bool is_dram) {
    mutex_alloc.lock();
    UInt64 phy_addr = static_cast<UInt64>(-1);

    if (is_dram) {
        UInt64 page_num = dram_buddy->allocate(PAGE_SIZE, 0, 0);
        if (page_num != static_cast<UInt64>(-1)) {
            phy_addr = page_num * PAGE_SIZE;
        }
    } else {
        UInt64 page_num = nvm_buddy->allocate(PAGE_SIZE, 0, 0);
        if (page_num != static_cast<UInt64>(-1)) {
            // NVM physical address = page_num * PAGE_SIZE + DRAM region size
            phy_addr = page_num * PAGE_SIZE + m_dram_size_bytes;
        }
    }

    if (phy_addr == static_cast<UInt64>(-1)) {
        mutex_alloc.unlock();
        return nullptr;
    }

    Hemem::hemem_page *page = create_active_page(phy_addr, is_dram);

    // --- Migration allocation stats ---
    if (is_dram) {
        alloc_stats.migration_alloc_dram++;
    } else {
        alloc_stats.migration_alloc_nvm++;
    }

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

        UInt64 phy_addr = static_cast<UInt64>(-1);
        if (is_dram) {
            UInt64 page_num = dram_buddy->allocate(PAGE_SIZE, 0, 0);
            if (page_num != static_cast<UInt64>(-1)) {
                phy_addr = page_num * PAGE_SIZE;
            }
        } else {
            UInt64 page_num = nvm_buddy->allocate(PAGE_SIZE, 0, 0);
            if (page_num != static_cast<UInt64>(-1)) {
                phy_addr = page_num * PAGE_SIZE + m_dram_size_bytes;
            }
        }

        if (phy_addr == static_cast<UInt64>(-1)) break;

        // 按需创建页元数据
        Hemem::hemem_page* page = create_active_page(phy_addr, is_dram);
        ret.push(page);

        // --- Migration allocation stats ---
        if (is_dram) {
            alloc_stats.migration_alloc_dram++;
        } else {
            alloc_stats.migration_alloc_nvm++;
        }
    }

    mutex_alloc.unlock();
    return ret;
}

void HememAllocator::deallocate(UInt64 region_begin, UInt64 core_id)
{
    // region_begin is a byte physical address.
    // Buddy->free() expects page numbers, so we convert: page_num = byte_addr / PAGE_SIZE.
    UInt64 page_start = region_begin / PAGE_SIZE;
    UInt64 page_end   = page_start; // single page

    mutex_alloc.lock();

    if (region_begin < m_dram_size_bytes) {
        // DRAM page: pass DRAM-relative page numbers directly
        dram_buddy->free(page_start, page_end);
        alloc_stats.dealloc_dram_pages++;
    } else {
        // NVM page: subtract DRAM page count to get NVM-relative page numbers
        UInt64 nvm_page_start = page_start - m_dram_size_pages;
        UInt64 nvm_page_end   = nvm_page_start;
        nvm_buddy->free(nvm_page_start, nvm_page_end);
        alloc_stats.dealloc_nvm_pages++;
    }

    destroy_active_page(region_begin);

    mutex_alloc.unlock();
}

void HememAllocator::deallocate(Hemem::hemem_page *page, bool is_dram, UInt64 core_id) {
    if (!page) return;

    UInt64 start_addr = page->phy_addr; // byte physical address

    // Safety: determine actual tier from address, not the caller's is_dram flag
    bool actual_dram = (start_addr < m_dram_size_bytes);

    // Convert to page number for buddy->free()
    UInt64 page_num = start_addr / PAGE_SIZE;

    mutex_alloc.lock();

    if (actual_dram) {
        dram_buddy->free(page_num, page_num);
        alloc_stats.dealloc_dram_pages++;
    } else {
        UInt64 nvm_page_num = page_num - m_dram_size_pages;
        nvm_buddy->free(nvm_page_num, nvm_page_num);
        alloc_stats.dealloc_nvm_pages++;
    }

    // Remove from m_active_pages to prevent stale entry reuse after migration swap.
    // After migration, page->phy_addr may differ from the key it was originally
    // inserted under, so we must search by the CURRENT phy_addr (start_addr).
    // Also try to remove by the key that might still map to this page.
    m_active_pages.erase(start_addr);

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
        is_dram_queue.pop();  // consume but not used — we determine tier from address

        if (!page) continue;

        UInt64 start_addr = page->phy_addr;  // byte physical address
        UInt64 page_num   = start_addr / PAGE_SIZE;

        // Determine actual tier from address
        bool actual_dram = (start_addr < m_dram_size_bytes);

        if (actual_dram) {
            dram_buddy->free(page_num, page_num);
            alloc_stats.dealloc_dram_pages++;
        } else {
            UInt64 nvm_page_num = page_num - m_dram_size_pages;
            nvm_buddy->free(nvm_page_num, nvm_page_num);
            alloc_stats.dealloc_nvm_pages++;
        }

        // Remove from active pages map to prevent stale entry reuse
        m_active_pages.erase(start_addr);

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