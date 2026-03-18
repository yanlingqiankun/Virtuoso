/*
 * HememAllocator — Tiered DRAM/NVM physical memory allocator.
 *
 * IMPORTANT: All physical addresses exposed by this allocator (phy_addr in
 * hemem_page, return value of allocate(), keys in m_active_pages, etc.) are
 * PAGE NUMBERS (PPN), NOT byte addresses.
 *
 * Physical address layout (in page numbers):
 *   DRAM region:  [0, m_dram_size_pages)
 *   NVM  region:  [m_dram_size_pages, m_dram_size_pages + m_nvm_size_pages)
 *
 * The buddy allocator returns DRAM-local or NVM-local page indices.
 * For NVM we add m_dram_size_pages to produce a globally unique PPN.
 */

#include "hemem_allocator.h"
#include "simulator.h"
#include "config.hpp"
#include "mimicos.h"
#include <iostream>

using namespace std;

static const UInt64 PAGE_SIZE = 4096;

HememAllocator::HememAllocator(String name, UInt64 dram_size, UInt64 nvm_size,
                               int max_order, int kernel_size, String frag_type)
    : PhysicalMemoryAllocator(name, dram_size + nvm_size, kernel_size)
{
    // Distribute kernel reservation across DRAM and NVM.
    // kernel_size is in MB; it must be carved out before user pages are available.
    UInt64 dram_kernel = 0;
    UInt64 nvm_kernel  = 0;

    if (dram_size >= (UInt64)kernel_size) {
        // Enough DRAM to hold the entire kernel region
        dram_kernel = kernel_size;
        nvm_kernel  = 0;
    } else {
        // DRAM is smaller than kernel — put what fits in DRAM, rest in NVM
        dram_kernel = dram_size;          // may be 0
        nvm_kernel  = kernel_size - dram_size;
        std::cout << "[Hemem] Kernel size (" << kernel_size
                  << " MB) exceeds DRAM size (" << dram_size
                  << " MB). Spilling " << nvm_kernel << " MB kernel to NVM." << std::endl;
    }

    std::cout << "[Hemem] Creating Buddy Allocators (Lazy Page Construction)" << std::endl;

    dram_buddy = new Buddy(dram_size, max_order, dram_kernel, frag_type);
    nvm_buddy  = new Buddy(nvm_size,  max_order, nvm_kernel,  frag_type);

    // All sizes stored as page counts
    m_dram_size_pages = (UInt64)dram_size * 1024 * 1024 / PAGE_SIZE;
    m_nvm_size_pages  = (UInt64)nvm_size  * 1024 * 1024 / PAGE_SIZE;

    // Only reserve DRAM for migration if migration is enabled
    if (Sim()->getCfg()->hasKey("migration/migration_enable") &&
        Sim()->getCfg()->getBool("migration/migration_enable")) {
        m_migration_enabled = true;
        // Fixed reservation in pages; read directly from config, default to 65536 pages (256 MB)
        this->dram_reserved_threshold = 256;
        if (Sim()->getCfg()->hasKey("migration/dram_reserved_pages"))
            this->dram_reserved_threshold = (UInt64)Sim()->getCfg()->getInt("migration/dram_reserved_pages");
        std::cout << "[Hemem] Migration enabled: reserving "
                  << this->dram_reserved_threshold << " DRAM pages for migration" << std::endl;
    } else {
        m_migration_enabled = false;
        this->dram_reserved_threshold = 0;
        std::cout << "[Hemem] Migration disabled: no DRAM pages reserved" << std::endl;
    }
    this->m_preferred_node = Sim()->getCfg()->getInt("perf_model/hemem_allocator/preferred_node");

    std::cout << "[Hemem] Allocator Initialized with preferred mem node "
              << this->m_preferred_node << ". Metadata will be created on-demand." << std::endl;

    // --- Initialize allocation statistics ---
    bzero(&alloc_stats, sizeof(alloc_stats));
    registerStatsMetric(name, 0, "alloc_dram_pages",     &alloc_stats.alloc_dram_pages);
    registerStatsMetric(name, 0, "alloc_nvm_pages",      &alloc_stats.alloc_nvm_pages);
    registerStatsMetric(name, 0, "migration_alloc_dram", &alloc_stats.migration_alloc_dram);
    registerStatsMetric(name, 0, "migration_alloc_nvm",  &alloc_stats.migration_alloc_nvm);
    registerStatsMetric(name, 0, "dealloc_dram_pages",   &alloc_stats.dealloc_dram_pages);
    registerStatsMetric(name, 0, "dealloc_nvm_pages",    &alloc_stats.dealloc_nvm_pages);
    registerStatsMetric(name, 0, "alloc_failed_oom",     &alloc_stats.alloc_failed_oom);
}

HememAllocator::~HememAllocator() {
    delete dram_buddy;
    delete nvm_buddy;
    for (auto& pair : m_active_pages) {
        delete pair.second;
    }
    m_active_pages.clear();
}

// phy_addr here is a PAGE NUMBER (PPN)
Hemem::hemem_page* HememAllocator::create_active_page(UInt64 phy_addr, bool is_dram) {
    auto it = m_active_pages.find(phy_addr);
    if (it != m_active_pages.end()) {
        Hemem::hemem_page* p = it->second;
        if (p->phy_addr != phy_addr) {
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
    p->phy_addr = phy_addr;       // PAGE NUMBER, not byte address
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

std::pair<UInt64, UInt64> HememAllocator::allocate(UInt64 bytes, UInt64 address,
                                                    UInt64 core_id, bool is_pagetable_allocation) {
    if (is_pagetable_allocation) {
        UInt64 physical_page = handle_page_table_allocations(bytes);
        return make_pair(physical_page, 12);
    }
    else {
        mutex_alloc.lock();

        UInt64 ppn = static_cast<UInt64>(-1);  // page number result
        bool is_in_dram = false;

        // buddy->allocate() returns a local page index within its region.
        // DRAM PPN = local_page_num
        // NVM  PPN = local_page_num + m_dram_size_pages
        if (m_preferred_node == 0) {
            // --- DRAM first ---
            if (dram_buddy->getFreePages() > this->dram_reserved_threshold) {
                UInt64 page_num = dram_buddy->allocate(bytes, 0, core_id);
                if (page_num != static_cast<UInt64>(-1)) {
                    ppn = page_num;
                    is_in_dram = true;
                }
            }
            if (ppn == static_cast<UInt64>(-1)) {
                UInt64 page_num = nvm_buddy->allocate(bytes, 0, core_id);
                if (page_num != static_cast<UInt64>(-1)) {
                    ppn = page_num + m_dram_size_pages;
                    is_in_dram = false;
                }
            }
        }
        else {
            // --- NVM first ---
            UInt64 page_num = nvm_buddy->allocate(bytes, 0, core_id);
            if (page_num != static_cast<UInt64>(-1)) {
                ppn = page_num + m_dram_size_pages;
                is_in_dram = false;
            }
            else if (dram_buddy->getFreePages() > this->dram_reserved_threshold) {
                page_num = dram_buddy->allocate(bytes, 0, core_id);
                if (page_num != static_cast<UInt64>(-1)) {
                    ppn = page_num;
                    is_in_dram = true;
                }
            }
        }

        if (ppn == static_cast<UInt64>(-1)) {
            std::cerr << "[Hemem] OUT OF MEMORY!!!!" << std::endl;
            alloc_stats.alloc_failed_oom++;
            mutex_alloc.unlock();
            assert(false);
            return make_pair(-1, 0);
        }

        Hemem::hemem_page *page = create_active_page(ppn, is_in_dram);

        mutex_alloc.unlock();

        page->vaddr = address & BASE_PAGE_MASK;
        if (m_migration_enabled)
            Sim()->getMimicOS()->getPageMigrationHandler()->page_fault(address & BASE_PAGE_MASK, page);

        if (is_in_dram) alloc_stats.alloc_dram_pages++;
        else            alloc_stats.alloc_nvm_pages++;

        // --- Real-time Memory Monitoring (every 256 allocations / ~1MB) ---
        if ((alloc_stats.alloc_dram_pages + alloc_stats.alloc_nvm_pages) % 256 == 0) {
            UInt64 used_dram = dram_buddy->getTotalPages() - dram_buddy->getFreePages();
            UInt64 used_nvm  = nvm_buddy->getTotalPages()  - nvm_buddy->getFreePages();
            std::cout << "[Hemem Monitor] "
                      << "DRAM Usage: " << (used_dram * 4096 / 1024 / 1024) << " MB ("
                      << used_dram << " pages) | "
                      << "NVM Usage: "  << (used_nvm  * 4096 / 1024 / 1024) << " MB ("
                      << used_nvm  << " pages) | "
                      << "DRAM Free: "  << dram_buddy->getFreePages() << " pages"
                      << std::endl;
        }

        return make_pair(ppn, 12);
    }
}

Hemem::hemem_page *HememAllocator::getAFreePage(bool is_dram) {
    mutex_alloc.lock();
    UInt64 ppn = static_cast<UInt64>(-1);

    if (is_dram) {
        UInt64 page_num = dram_buddy->allocate(PAGE_SIZE, 0, 0);
        if (page_num != static_cast<UInt64>(-1))
            ppn = page_num;
    } else {
        UInt64 page_num = nvm_buddy->allocate(PAGE_SIZE, 0, 0);
        if (page_num != static_cast<UInt64>(-1))
            ppn = page_num + m_dram_size_pages;
    }

    if (ppn == static_cast<UInt64>(-1)) {
        mutex_alloc.unlock();
        return nullptr;
    }

    Hemem::hemem_page *page = create_active_page(ppn, is_dram);

    if (is_dram) alloc_stats.migration_alloc_dram++;
    else         alloc_stats.migration_alloc_nvm++;

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

        UInt64 ppn = static_cast<UInt64>(-1);
        if (is_dram) {
            UInt64 page_num = dram_buddy->allocate(PAGE_SIZE, 0, 0);
            if (page_num != static_cast<UInt64>(-1))
                ppn = page_num;
        } else {
            UInt64 page_num = nvm_buddy->allocate(PAGE_SIZE, 0, 0);
            if (page_num != static_cast<UInt64>(-1))
                ppn = page_num + m_dram_size_pages;
        }

        if (ppn == static_cast<UInt64>(-1)) break;

        Hemem::hemem_page* page = create_active_page(ppn, is_dram);
        ret.push(page);

        if (is_dram) alloc_stats.migration_alloc_dram++;
        else         alloc_stats.migration_alloc_nvm++;
    }

    mutex_alloc.unlock();
    return ret;
}

void HememAllocator::deallocate(UInt64 region_begin, UInt64 core_id)
{
    // region_begin is a PAGE NUMBER (PPN).
    UInt64 ppn = region_begin;

    mutex_alloc.lock();

    if (ppn < m_dram_size_pages) {
        dram_buddy->free(ppn, ppn);
        alloc_stats.dealloc_dram_pages++;
    } else {
        UInt64 nvm_local = ppn - m_dram_size_pages;
        nvm_buddy->free(nvm_local, nvm_local);
        alloc_stats.dealloc_nvm_pages++;
    }

    destroy_active_page(ppn);

    mutex_alloc.unlock();
}

void HememAllocator::deallocate(Hemem::hemem_page *page, bool is_dram, UInt64 core_id) {
    if (!page) return;

    UInt64 ppn = page->phy_addr;  // page number

    // Determine actual tier from PPN, not the caller's is_dram flag
    bool actual_dram = (ppn < m_dram_size_pages);

    mutex_alloc.lock();

    if (actual_dram) {
        dram_buddy->free(ppn, ppn);
        alloc_stats.dealloc_dram_pages++;
    } else {
        UInt64 nvm_local = ppn - m_dram_size_pages;
        nvm_buddy->free(nvm_local, nvm_local);
        alloc_stats.dealloc_nvm_pages++;
    }

    m_active_pages.erase(ppn);

    page->present = false;
    page->naccesses = 0;
    page->migrating = false;

    mutex_alloc.unlock();
}

void HememAllocator::deallocatePages(std::queue<Hemem::hemem_page*> pages,
                                      std::queue<bool> is_dram_queue, UInt64 app_id) {
    mutex_alloc.lock();
    while (!pages.empty() && !is_dram_queue.empty()) {
        Hemem::hemem_page *page = pages.front();
        pages.pop();
        is_dram_queue.pop();

        if (!page) continue;

        UInt64 ppn = page->phy_addr;  // page number
        bool actual_dram = (ppn < m_dram_size_pages);

        if (actual_dram) {
            dram_buddy->free(ppn, ppn);
            alloc_stats.dealloc_dram_pages++;
        } else {
            UInt64 nvm_local = ppn - m_dram_size_pages;
            nvm_buddy->free(nvm_local, nvm_local);
            alloc_stats.dealloc_nvm_pages++;
        }

        m_active_pages.erase(ppn);

        page->present = false;
        page->naccesses = 0;
        page->migrating = false;
    }
    mutex_alloc.unlock();
}

std::vector<Range> HememAllocator::allocate_ranges(IntPtr start_va, IntPtr end_va, int app_id)
{
    std::vector<Range> ranges;
    return ranges;
}

void HememAllocator::fragment_memory()
{
    return;
}
