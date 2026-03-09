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
#define DEFAULT_COLD_THRESHOLD 1          // Access count at or below which DRAM pages are considered cold
#define DEFAULT_EPOCHS_PER_DECAY 4        // Number of epochs per one halving of hotness

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
          cold_threshold(DEFAULT_COLD_THRESHOLD),
          epochs_per_decay(DEFAULT_EPOCHS_PER_DECAY),
          proactive_demotions(0),
          direct_promotions(0)
    {
        this->setName("Memtis");
        std::random_device rd;
        rng = std::mt19937(rd());
        if (Sim()->getCfg()->hasKey("migration/hot_threshold")) {
            hot_threshold = Sim()->getCfg()->getInt("migration/hot_threshold");
        }
        if (Sim()->getCfg()->hasKey("migration/cold_threshold")) {
            cold_threshold = Sim()->getCfg()->getInt("migration/cold_threshold");
        }
        if (Sim()->getCfg()->hasKey("migration/epochs_per_decay")) {
            epochs_per_decay = Sim()->getCfg()->getInt("migration/epochs_per_decay");
        }
        if (epochs_per_decay == 0) epochs_per_decay = 1; // Guard against division by zero
        std::cout << "[Memtis] Decoupled mode: hot_threshold=" << hot_threshold
                  << ", cold_threshold=" << cold_threshold
                  << ", epochs_per_decay=" << epochs_per_decay << std::endl;
        registerStatsMetric("memtis", 0, "proactive_demotions", &proactive_demotions);
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
            UInt64 raw_delta = global_epoch - page->local_clock;

            // Convert raw epoch delta into effective decay shifts
            UInt64 effective_shifts = raw_delta / epochs_per_decay;

            // Avoid undefined behavior with large bit shifts (>= 64)
            if (effective_shifts >= 64) {
                page->naccesses = 0;
            } else if (effective_shifts > 0) {
                // Decay hotness: Divide by 2^effective_shifts (Right shift)
                // This simulates the periodic halving in Memtis, but at a
                // more reasonable pace controlled by epochs_per_decay.
                page->naccesses >>= effective_shifts;
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
    // Manages the global epoch. Promotion and Demotion are fully decoupled:
    //   Phase 1: Proactive Demotion — independently identifies cold DRAM pages and demotes them.
    //   Phase 2: Promotion — promotes hot NVM pages only when DRAM has free space.
    void Memtis::policy() {
        while (still_run) {
            usleep(policy_interval_us);

            // 1. Update Global Epoch (Tick-Tock)
            // Implicitly, all pages are now "older" and will be decayed upon next access/sample.
            current_epoch.fetch_add(1, std::memory_order_relaxed);

            // ============================================================
            // Phase 1: Proactive Demotion (Independent of Promotion)
            // Sample DRAM pages, find cold ones, and demote them to NVM.
            // This frees up DRAM space regardless of whether promotions are pending.
            // ============================================================
            {
                std::vector<hemem_page_t*> cold_pages;
                {
                    std::shared_lock<std::shared_mutex> list_lock(page_list_mutex);

                    if (!dram_pages.empty()) {
                        // Random sample from DRAM pages
                        std::vector<hemem_page_t*> dram_sample;
                        size_t actual_sample = std::min(sample_size, dram_pages.size());
                        std::uniform_int_distribution<size_t> dist(0, dram_pages.size() - 1);

                        // Use a set to avoid sampling the same page twice
                        std::set<hemem_page_t*> sampled;
                        for (size_t i = 0; i < actual_sample; ++i) {
                            hemem_page_t* p = dram_pages[dist(rng)];
                            if (sampled.insert(p).second) {
                                dram_sample.push_back(p);
                            }
                        }

                        // Identify cold pages: hotness at or below cold_threshold
                        for (auto* p : dram_sample) {
                            if (!p->migrating && get_current_hotness(p) <= cold_threshold) {
                                cold_pages.push_back(p);
                            }
                        }

                        // Sort by hotness ascending (coldest first), limit to batch_size
                        if (cold_pages.size() > 1) {
                            std::sort(cold_pages.begin(), cold_pages.end(),
                                [&](hemem_page_t* a, hemem_page_t* b) {
                                    return get_current_hotness(a) < get_current_hotness(b);
                                });
                        }
                        size_t demote_limit = batch_size.load();
                        if (cold_pages.size() > demote_limit) {
                            cold_pages.resize(demote_limit);
                        }
                    }
                }

                // Execute demotion
                if (!cold_pages.empty()) {
                    std::vector<hemem_page_t*> empty_promote;
                    batch_migrate(empty_promote, cold_pages);
                    proactive_demotions += cold_pages.size();

                    // Update global lists
                    std::unique_lock<std::shared_mutex> list_lock(page_list_mutex);

                    // Remove successfully demoted pages from dram_pages
                    auto d_it = std::remove_if(dram_pages.begin(), dram_pages.end(),
                        [](hemem_page_t* p) { return !p->in_dram; });
                    dram_pages.erase(d_it, dram_pages.end());

                    // Add demoted pages to nvm_pages
                    for (auto* p : cold_pages) {
                        if (!p->in_dram) nvm_pages.push_back(p);
                        // else: migration failed, page stays in DRAM (already in dram_pages)
                    }
                }
            }

            // ============================================================
            // Phase 2: Promotion (Independent of Demotion)
            // Process fast promotion queue. Only promote if DRAM has free
            // space (freed by Phase 1 or initial allocation).
            // Pages that cannot be promoted are re-queued for the next cycle.
            // ============================================================
            {
                std::vector<hemem_page_t*> to_promote;

                // Fetch candidates from the fast promotion queue
                {
                    std::lock_guard<std::mutex> q_lock(queue_mutex);
                    if (!fast_promotion_queue.empty()) {
                        size_t batch = batch_size.load();
                        size_t count = std::min(batch, fast_promotion_queue.size());

                        to_promote.assign(fast_promotion_queue.begin(),
                                          fast_promotion_queue.begin() + count);
                        fast_promotion_queue.erase(fast_promotion_queue.begin(),
                                                   fast_promotion_queue.begin() + count);
                    }
                }

                if (to_promote.empty()) continue; // Nothing to promote, next cycle

                // Check available DRAM space
                HememAllocator* allocator = dynamic_cast<HememAllocator*>(
                    Sim()->getMimicOS()->getMemoryAllocator());
                size_t dram_free = allocator ? allocator->getDramFreePages() : 0;

                // Split: promote only as many as free space allows
                std::vector<hemem_page_t*> can_promote;
                std::vector<hemem_page_t*> cannot_promote;

                for (size_t i = 0; i < to_promote.size(); ++i) {
                    if (i < dram_free) {
                        can_promote.push_back(to_promote[i]);
                    } else {
                        cannot_promote.push_back(to_promote[i]);
                    }
                }

                // Execute promotion
                if (!can_promote.empty()) {
                    direct_promotions += can_promote.size();

                    std::vector<hemem_page_t*> empty_demote;
                    batch_migrate(can_promote, empty_demote);

                    // Update global lists
                    std::unique_lock<std::shared_mutex> list_lock(page_list_mutex);
                    std::lock_guard<std::mutex> q_lock(queue_mutex);

                    // Remove successfully promoted pages from nvm_pages
                    auto n_it = std::remove_if(nvm_pages.begin(), nvm_pages.end(),
                        [](hemem_page_t* p) { return p->in_dram; });
                    nvm_pages.erase(n_it, nvm_pages.end());

                    for (auto* p : can_promote) {
                        if (p->in_dram) dram_pages.push_back(p);
                        else nvm_pages.push_back(p); // Migration failed, keep in NVM

                        pending_pages.erase(p);
                        p->migrating = false;
                    }
                }

                // Re-queue pages that couldn't be promoted (DRAM full).
                // They will be retried in the next policy cycle after demotion frees space.
                if (!cannot_promote.empty()) {
                    std::lock_guard<std::mutex> q_lock(queue_mutex);
                    fast_promotion_queue.insert(fast_promotion_queue.begin(),
                        cannot_promote.begin(), cannot_promote.end());
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