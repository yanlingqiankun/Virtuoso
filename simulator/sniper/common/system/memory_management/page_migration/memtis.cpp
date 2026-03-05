//
// Created by user on 25-11-25.
//

#include "memtis.h"
#include "simulator.h"
#include "mimicos.h"
#include "hemem_allocator.h"
#include <unistd.h>
#include <iostream>
#include <cmath>

#include "../../../core/core.h"

// Default configuration values
#define DEFAULT_SAMPLE_SIZE 1024
#define DEFAULT_SCAN_INTERVAL 1000000      // 1s
#define DEFAULT_POLICY_INTERVAL 1000000  // 1s
#define DEFAULT_HOT_THRESHOLD 2          // Access count to trigger fast promotion

namespace Hemem {

    Memtis::Memtis()
        : PageMigration(),
          still_run(true),
          current_epoch(0), // Initialize global epoch
          batch_size(32),
          sample_size(DEFAULT_SAMPLE_SIZE),
          scan_interval_us(DEFAULT_SCAN_INTERVAL),
          policy_interval_us(DEFAULT_POLICY_INTERVAL),
          hot_threshold(DEFAULT_HOT_THRESHOLD),
          hotness_margin(0),
          hotness_guard_skipped(0),
          direct_promotions(0)
    {
        this->setName("Memtis");
        std::random_device rd;
        rng = std::mt19937(rd());
        if (Sim()->getCfg()->hasKey("migration/hot_threshold")) {
            hot_threshold = Sim()->getCfg()->getInt("migration/hot_threshold");
        }
        if (Sim()->getCfg()->hasKey("migration/hotness_margin")) {
            hotness_margin = Sim()->getCfg()->getInt("migration/hotness_margin");
        }
        std::cout << "[Memtis] Hotness Guard enabled with margin = " << hotness_margin << std::endl;
        registerStatsMetric("memtis", 0, "hotness_guard_skipped", &hotness_guard_skipped);
        registerStatsMetric("memtis", 0, "direct_promotions", &direct_promotions);
    }

    Memtis::~Memtis() { stop(); }

    void Memtis::setBatchSize(size_t size) { batch_size = size; }
    void Memtis::setHotThreshold(UInt64 threshold) { hot_threshold = threshold; }

    // === Lazy Cooling Logic ===
    // This function acts as the "Exponential Moving Average" (EMA) decay mechanism.
    // Instead of iterating all pages, we calculate the decay only when we touch the page.
    void Memtis::lazy_cool(hemem_page_t* page, UInt64 global_epoch) {
        // If the page's local timestamp is behind the global time
        if (page->local_clock < global_epoch) {
            UInt64 delta = global_epoch - page->local_clock;

            // Avoid undefined behavior with large bit shifts (>= 64)
            if (delta >= 64) {
                page->naccesses = 0;
            } else {
                // Decay hotness: Divide by 2^delta (Right shift)
                // This simulates the periodic halving in Memtis
                page->naccesses >>= delta;
            }
            // Sync page time to global time
            page->local_clock = global_epoch;
        }
    }

    // Helper to get hotness score. IMPORTANT: Must cool before returning.
    // Otherwise, an old high count looks hotter than a recent moderate count.
    UInt64 Memtis::get_current_hotness(hemem_page_t* page) {
        // Use relaxed memory order for performance; strict consistency isn't required for heuristics.
        lazy_cool(page, current_epoch.load(std::memory_order_relaxed));
        return page->naccesses;
    }

    void Memtis::page_fault(UInt64 laddr, void *ptr) {
        hemem_page_t *page = static_cast<hemem_page_t *>(ptr);
        std::unique_lock<std::shared_mutex> lock(page_list_mutex);

        auto const& [it, inserted] = all_pages_map.insert({laddr, page});
        if (inserted) {
            page->naccesses = 0;
            page->migrating = false; // Flag to prevent duplicate queuing

            // Initialize local clock to current epoch
            page->local_clock = current_epoch.load(std::memory_order_relaxed);

            if (page->in_dram) dram_pages.push_back(page);
            else nvm_pages.push_back(page);
        }
    }

    // === Scan Thread (Producer) ===
    // Iterates over hardware events (PEBS/IBS), updates counters, and identifies hot pages.
    void Memtis::scan() {
        bool perf_empty = false;
        size_t core_number = Sim()->getCoreManager()->getCoreNum();

        while (still_run) {
            // Iterate over all cores to collect samples
            for (int i = 0; i < core_number; ++i) {
                PageTracer *page_tracer = Sim()->getCoreManager()->getCoreFromID(i)->getPageTracer();

                while (true) {
                    PerfSample page_sample = page_tracer->getPerfSample(&perf_empty);
                    // cout << "perf_empty = " << perf_empty << endl;
                    if (perf_empty) break;

                    // Calculate Base Page address
                    UInt64 addr = page_sample.addr & pages_mask[0];
                    hemem_page_t *page = nullptr;
                    {
                        std::shared_lock<std::shared_mutex> lock(page_list_mutex);
                        // Fast lock-free lookup (assuming map stability)
                        auto it = all_pages_map.find(addr);
                        if (it == all_pages_map.end()) continue;

                        page = it->second;
                        // cout << "find pages = " << page->vaddr << endl;
                    }
                    // 1. Lazy Cool: Before adding new access, decay the old value based on elapsed time.
                    lazy_cool(page, current_epoch.load(std::memory_order_relaxed));

                    // 2. Increment Unified Access Counter (Read & Write treated equally)
                    if (page_sample.type == LLC_Miss_RD || page_sample.type == LLC_Miss_ST) {
                        page->naccesses++;
                        
                        // Track accesses to pages that have been migrated from NVM to DRAM.
                        if (page->in_dram && !page->initial_in_dram) {
                            Sim()->getMimicOS()->incrementBeneficialDramAccessSamples();
                        }
                        
                        // Track accesses to pages that were demoted down to NVM (Penalty)
                        if (!page->in_dram && page->initial_in_dram) {
                            Sim()->getMimicOS()->incrementPenalizedNvmAccessSamples();
                        }
                    } else {
                        continue;
                    }

                    // 3. Fast Path Promotion Check
                    // If page is in NVM (Slow Tier) and hotness exceeds threshold, queue it immediately.
                    if (!page->in_dram && !page->migrating) {
                        if (page->naccesses >= hot_threshold) {
                            std::lock_guard<std::mutex> q_lock(queue_mutex);

                            // Check uniqueness to avoid double-queuing
                            if (pending_pages.find(page) == pending_pages.end()) {
                                fast_promotion_queue.push_back(page);
                                pending_pages.insert(page);
                                page->migrating = true; // Mark as "in-flight"
                            }
                        }
                    }
                }
            }
            usleep(scan_interval_us);
        }
    }

    // === Policy Thread (Consumer) ===
    // Manages the global epoch, processes the promotion queue, and selects victims.
    void Memtis::policy() {
        while (still_run) {
            usleep(policy_interval_us);

            // 1. Update Global Epoch (Tick-Tock)
            // Implicitly, all pages are now "older" and will be decayed upon next access/sample.
            current_epoch.fetch_add(1, std::memory_order_relaxed);
            // cout << "current_echoch = " << current_epoch << endl;

            std::vector<hemem_page_t*> to_promote;

            // 2. Fetch candidates from Fast Promotion Queue
            {
                std::lock_guard<std::mutex> q_lock(queue_mutex);
                if (fast_promotion_queue.empty()) continue; // Nothing to do

                size_t batch = batch_size.load();
                size_t count = std::min(batch, fast_promotion_queue.size());

                // Move pages from queue to local vector
                to_promote.assign(fast_promotion_queue.begin(), fast_promotion_queue.begin() + count);
                fast_promotion_queue.erase(fast_promotion_queue.begin(), fast_promotion_queue.begin() + count);
            }
            // cout << "Number of to_promote is " << to_promote.size() << endl;

            // --- Step 3: Check DRAM free space and split into direct promotions vs swaps ---
            HememAllocator* allocator = dynamic_cast<HememAllocator*>(
                Sim()->getMimicOS()->getMemoryAllocator());

            size_t dram_free = allocator ? allocator->getDramFreePages() : 0;

            // Candidates that can be directly promoted (DRAM has free space, no swap needed)
            std::vector<hemem_page_t*> direct_promote;
            // Candidates that need a swap (DRAM is full for these)
            std::vector<hemem_page_t*> swap_candidates;

            // Split: first N go direct (where N = dram_free), rest need swap
            for (size_t i = 0; i < to_promote.size(); ++i) {
                if (i < dram_free) {
                    direct_promote.push_back(to_promote[i]);
                } else {
                    swap_candidates.push_back(to_promote[i]);
                }
            }

            // === Path A: Direct Promotion (no demotion needed) ===
            if (!direct_promote.empty()) {
                direct_promotions += direct_promote.size();

                // batch_migrate with empty demote vector = promote only
                std::vector<hemem_page_t*> empty_demote;
                batch_migrate(direct_promote, empty_demote);

                // Maintenance: update global lists
                std::unique_lock<std::shared_mutex> list_lock(page_list_mutex);
                std::lock_guard<std::mutex> q_lock(queue_mutex);

                // Move promoted pages from nvm_pages to dram_pages
                auto n_it = std::remove_if(nvm_pages.begin(), nvm_pages.end(),
                    [](hemem_page_t* p) { return p->in_dram; });
                nvm_pages.erase(n_it, nvm_pages.end());

                for (auto* p : direct_promote) {
                    if (p->in_dram) dram_pages.push_back(p);
                    else nvm_pages.push_back(p); // Migration failed? keep in NVM

                    pending_pages.erase(p);
                    p->migrating = false;
                }
            }

            // === Path B: Swap Promotion (DRAM full, need to find victims) ===
            if (!swap_candidates.empty()) {
                std::vector<hemem_page_t*> to_demote;

                // 3B. Select Victims in DRAM via Random Sampling + Sorting
                {
                    std::unique_lock<std::shared_mutex> list_lock(page_list_mutex);

                    if (!dram_pages.empty()) {
                        std::vector<hemem_page_t*> dram_sample;
                        size_t actual_sample = std::min(sample_size, dram_pages.size());
                        std::uniform_int_distribution<size_t> dist(0, dram_pages.size() - 1);

                        for (size_t i = 0; i < actual_sample; ++i) {
                            dram_sample.push_back(dram_pages[dist(rng)]);
                        }

                        std::sort(dram_sample.begin(), dram_sample.end(),
                            [&](hemem_page_t* a, hemem_page_t* b) {
                                return get_current_hotness(a) < get_current_hotness(b);
                            });

                        size_t demote_cnt = std::min(swap_candidates.size(), dram_sample.size());
                        for (size_t i = 0; i < demote_cnt; ++i) {
                            to_demote.push_back(dram_sample[i]);
                        }
                    }
                }

                // 4B. Hotness Guard: Only swap if promote candidate is significantly hotter
                std::vector<hemem_page_t*> final_promote;
                std::vector<hemem_page_t*> final_demote;
                std::vector<hemem_page_t*> skipped_promote;

                {
                    size_t pair_count = std::min(swap_candidates.size(), to_demote.size());
                    for (size_t i = 0; i < pair_count; ++i) {
                        UInt64 promote_hotness = get_current_hotness(swap_candidates[i]);
                        UInt64 demote_hotness  = get_current_hotness(to_demote[i]);

                        if (promote_hotness > demote_hotness + hotness_margin) {
                            final_promote.push_back(swap_candidates[i]);
                            final_demote.push_back(to_demote[i]);
                        } else {
                            skipped_promote.push_back(swap_candidates[i]);
                            hotness_guard_skipped++;
                        }
                    }
                    // Unmatched swap candidates (not enough victims)
                    for (size_t i = pair_count; i < swap_candidates.size(); ++i) {
                        skipped_promote.push_back(swap_candidates[i]);
                    }
                }

                // 5B. Execute Batch Migration (only the filtered pairs)
                if (!final_promote.empty()) {
                    batch_migrate(final_promote, final_demote);

                    // 6B. Maintenance: Update Global Lists after Migration
                    std::unique_lock<std::shared_mutex> list_lock(page_list_mutex);
                    std::lock_guard<std::mutex> q_lock(queue_mutex);

                    auto is_nvm = [](hemem_page_t* p) { return !p->in_dram; };
                    auto is_dram = [](hemem_page_t* p) { return p->in_dram; };

                    auto d_it = std::remove_if(dram_pages.begin(), dram_pages.end(), is_nvm);
                    dram_pages.erase(d_it, dram_pages.end());

                    auto n_it = std::remove_if(nvm_pages.begin(), nvm_pages.end(), is_dram);
                    nvm_pages.erase(n_it, nvm_pages.end());

                    for (auto* p : final_promote) {
                        if (p->in_dram) dram_pages.push_back(p);
                        else nvm_pages.push_back(p);

                        pending_pages.erase(p);
                        p->migrating = false;
                    }

                    for (auto* p : final_demote) {
                        if (!p->in_dram) nvm_pages.push_back(p);
                        else dram_pages.push_back(p);
                    }
                }

                // Release skipped swap candidates
                {
                    std::lock_guard<std::mutex> q_lock(queue_mutex);
                    for (auto* p : skipped_promote) {
                        pending_pages.erase(p);
                        p->migrating = false;
                    }
                }
            }
        }
    }

    hemem_page_t* Memtis::getPage(IntPtr vaddr) {
        std::shared_lock<std::shared_mutex> lock(page_list_mutex);
        auto it = all_pages_map.find(vaddr);
        if (it != all_pages_map.end()) {
            return it->second;
        }
        return nullptr;
    }

    void Memtis::batch_migrate(const std::vector<hemem_page_t*>& to_promote,
                               const std::vector<hemem_page_t*>& to_demote) {
        std::queue<hemem_page_t*> q;
        std::queue<bool> up;

        // Push promotions (Up to Fast Tier)
        for(auto* p : to_promote) { q.push(p); up.push(true); }
        // cout << "promotions : " << q.size() << endl;

        // Push demotions (Down to Capacity Tier)
        for(auto* p : to_demote) { q.push(p); up.push(false); }
        // cout << "migrations : " << q.size() << endl;
        if(!q.empty()) {
            Sim()->getMimicOS()->move_pages(q, up, 0);
        }
    }

    void Memtis::start() {
        still_run = true;
        std::cout << "[Memtis] Starting background threads..." << std::endl;
        scan_thread_handle = std::thread(&Memtis::scan, this);
        policy_thread_handle = std::thread(&Memtis::policy, this);
    }

    void Memtis::stop() {
        still_run = false;
        if (scan_thread_handle.joinable()) scan_thread_handle.join();
        if (policy_thread_handle.joinable()) policy_thread_handle.join();
        std::cout << "[Memtis] Stopped." << std::endl;
    }
}