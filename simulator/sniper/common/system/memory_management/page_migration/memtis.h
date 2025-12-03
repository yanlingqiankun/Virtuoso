//
// Created by user on 25-11-25.
//

#ifndef MEMTIS_H
#define MEMTIS_H

#include "hemem.h" // Include existing hemem structures (hemem_page_t, etc.)
#include <vector>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <random>
#include <map>
#include <set>

namespace Hemem {

    class Memtis : public PageMigration {
    private:
        // --- Data Structures ---

        // DRAM pages list: Used for random sampling to select demotion victims.
        std::vector<hemem_page_t*> dram_pages;

        // NVM pages list: Maintained for consistency, though NVM hotness is mostly handled via Fast Path.
        std::vector<hemem_page_t*> nvm_pages;

        // Fast lookup map: Maps virtual address to page metadata.
        std::map<UInt64, hemem_page_t*> all_pages_map;

        // --- Fast Path & Queue ---

        // Fast Promotion Queue: The Scan thread pushes detected hot NVM pages here immediately.
        std::vector<hemem_page_t*> fast_promotion_queue;

        // Mutex to protect the fast promotion queue.
        std::mutex queue_mutex;

        // Helper set to prevent duplicate entries in the pending queue.
        std::set<hemem_page_t*> pending_pages;

        // Hotness threshold: If naccesses >= this value, promote immediately.
        UInt64 hot_threshold;

        // --- Threading & Synchronization ---
        bool still_run;
        std::thread scan_thread_handle;
        std::thread policy_thread_handle;

        // Mutex to protect global lists (dram_pages, nvm_pages) and the map.
        std::mutex page_list_mutex;

        // --- Lazy Cooling State ---

        // Global logical clock (Epoch). Incremented by the Policy thread.
        // Used to calculate how much to decay a page's hotness.
        std::atomic<UInt64> current_epoch;

        // --- Configuration ---
        std::atomic<size_t> batch_size;    // Number of pages to migrate at once
        size_t sample_size;                // Number of DRAM pages to sample for victim selection
        UInt64 scan_interval_us;
        UInt64 policy_interval_us;
        std::mt19937 rng;                  // Random number generator for sampling

        // --- Internal Functions ---

        void scan();   // Producer: Monitors hardware events
        void policy(); // Consumer: Makes migration decisions

        // Lazy Cooling: Updates the page's hotness based on the time elapsed since last access.
        // Updates page->naccesses and page->local_clock.
        void lazy_cool(hemem_page_t* page, UInt64 global_epoch);

        // Helper to get normalized hotness (triggers lazy_cool internally).
        UInt64 get_current_hotness(hemem_page_t* page);

        // Interface to OS simulator for moving pages
        void batch_migrate(const std::vector<hemem_page_t*>& to_promote,
                           const std::vector<hemem_page_t*>& to_demote);

    public:
        Memtis();
        ~Memtis();

        void start() override;
        void stop() override;

        // Callback when a new page is allocated/faulted
        void page_fault(UInt64 laddr, void *ptr) override;

        // Configuration setters
        void setBatchSize(size_t size) override;
        void setHotThreshold(UInt64 threshold);
        hemem_page_t* getPage(IntPtr vaddr);
    };
}

#endif //MEMTIS_H