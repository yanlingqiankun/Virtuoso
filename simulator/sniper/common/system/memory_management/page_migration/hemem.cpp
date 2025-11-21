//
// Created by shado on 25-6-5.
//


#include "simulator.h"
#include "mimicos.h"
#include "page_fault_handler_base.h"
#include "hemem_allocator.h"
#include "hemem.h"
#include <queue>

#define PEBS_KSWAPD_INTERVAL      (100000) // in us (10ms)
#define PEBS_KSWAPD_MIGRATE_RATE  (10UL * 1024UL * 1024UL * 1024UL) // 10GB
#define HOT_READ_THRESHOLD        (1)
#define HOT_WRITE_THRESHOLD       (1)
#define PEBS_COOLING_THRESHOLD    (1)

#define HOT_RING_REQS_THRESHOLD   (1024*1024)
#define COLD_RING_REQS_THRESHOLD  (128)
#define CAPACITY                  (16*1024*1024)
#define COOLING_PAGES             (8192)

namespace Hemem{

    UInt64 global_clock;
    bool volatile need_cool_dram = false;
    bool volatile need_cool_nvm = false;
    UInt64 other_pages_cnt, hemem_pages_cnt;

    pageTypes pagesize_to_pt(int page_size) {
        switch (page_size) {
            case HUGE_PAGE_SIZE: return BASEP;
            case BASE_PAGE_SIZE: return HUGEP;
            default: return NPAGETYPES;
        }
    }

    UInt64 pt_to_pagesize(pageTypes pt)
    {
        switch(pt) {
            case HUGEP: return HUGE_PAGE_SIZE;
            case BASEP: return BASE_PAGE_SIZE;
            default: assert(!"Unknown page type");
        }
    }


    void enqueue(fifo_list *queue, hemem_page *entry) {
        // std::cout << __func__ << "\tenter" << std::endl;
        queue->lock();
        if (entry->prev != nullptr)
            std::cout << "--------queue is " << queue->name << "----------- entry's queue is " << entry->list->name << std::endl;
        assert(entry->prev == nullptr);
        entry->next = queue->first;
        if (queue->first != nullptr) {
            if (queue->first->prev != nullptr)
                std::cout << "--------queue is " << queue->name << "----------- entry's queue is " << entry->list->name << std::endl;
            assert(queue->first->prev == nullptr);
            queue->first->prev = entry;
        } else {
            assert(queue->last == nullptr);
            assert(queue->empty());
            queue->last = entry;
        }

        queue->first = entry;
        entry->list = queue;
        queue->numentries++;
        queue->unlock();
        // std::cout << __func__ << "\texit" << std::endl;
    }

    hemem_page *dequeue(fifo_list *queue) {
        // std::cout << __func__ << "\tenter" << std::endl;
        queue->lock();
        hemem_page *ret = queue->last;
        if (ret == nullptr) {
            queue->unlock();
            return ret;
        }

        queue->last = ret->prev;
        if (queue->last != nullptr) {queue->last->next = nullptr;}
        else {queue->first = nullptr;}

        ret->prev = ret->next = nullptr;
        ret->list = nullptr;

        assert(!queue->empty());
        queue->numentries--;
        queue->unlock();
        // std::cout << __func__ << "\texit" << std::endl;
        return ret;
    }


    void page_list_remove_page(fifo_list *list, hemem_page *page) {
        if (list->first == nullptr) {
            assert(list->empty());
            return;
        }

        if (list->first == page) {
            list->first = page->next;
        }

        if (list->last == page) {
            list->last = page->prev;
        }

        if (page->next != nullptr) {
            page->next->prev = page->prev;
        }

        if (page->prev != nullptr) {
            page->prev->next = page->next;
        }

        assert(!list->empty());
        list->numentries--;
        page->next = nullptr;
        page->prev = nullptr;
        page->list = nullptr;
    }


    hemem_page* nextPage(fifo_list *list, hemem_page *page) {
        hemem_page *next_page = nullptr;

        if (page == nullptr) {
            next_page = list->last;
        } else {
            next_page = page->prev;
            assert(page->list == list);
        }
        return next_page;
    }

    void updateCurrentCoolPage(hemem_page **cur_cool_in_dram, hemem_page **cur_cool_in_nvm, hemem_page* page) {
        if (page == nullptr) return;
        if (page == *cur_cool_in_dram) *cur_cool_in_dram = nextPage(page->list, page);
        if (page == *cur_cool_in_nvm) *cur_cool_in_nvm = nextPage(page->list, page);
    }

    void Hemem::makeHot(hemem_page *page) {
        if (page->hot) return;

        if (page->in_dram) {
            assert(page->list == &dram_cold_list);
            page_list_remove_page(&dram_cold_list, page);
            page->hot = true;
            // std::cout << "[Hemem] inserted " << page->prev << " in " << __LINE__ <<std::endl;
            enqueue(&dram_hot_list, page);
        } else {
            assert(page->list == &nvm_cold_list);
            page_list_remove_page(&nvm_cold_list, page);
            page->hot = true;
            // std::cout << "[Hemem] inserted " << page->prev << " in " << __LINE__ <<std::endl;
            enqueue(&nvm_hot_list, page);
        }
    }

    void Hemem::makeCold(hemem_page *page) {
        assert(page != nullptr);
        assert(page->vaddr != 0);

        if (!page->hot) {
            if (page->in_dram) assert(page->list == &dram_cold_list);
            else assert(page->list == &nvm_cold_list);
            return;
        }

        if (page->in_dram) {
            assert(page->list == &dram_hot_list);
            page_list_remove_page(&dram_hot_list, page);
            page->hot = false;
            enqueue(&dram_cold_list, page);
        } else {
            assert(page->list == &nvm_hot_list);
            page_list_remove_page(&nvm_hot_list, page);
            enqueue(&nvm_cold_list, page);
        }

    }

    hemem_page *partial_cool_peek_and_move(fifo_list *hot, fifo_list *cold,  bool dram, hemem_page* current) {
        hemem_page *p;
        uint64_t tmp_accesses[NPBUFTYPES];
        static hemem_page* start_dram_page = nullptr;
        static hemem_page* start_nvm_page = nullptr;

        if (dram && !need_cool_dram) {
            return current;
        }
        if (!dram && !need_cool_nvm) {
            return current;
        }

        if (start_dram_page == nullptr && dram) {
            start_dram_page = hot->last;
        }

        if (start_nvm_page == nullptr && !dram) {
            start_nvm_page = hot->last;
        }

        for (int i = 0; i < COOLING_PAGES; i++) {
            p = nextPage(hot, current);
            if (p == nullptr) {
                break;
            }
            if (dram) {
                assert(p->in_dram);
            }
            else {
                assert(!p->in_dram);
            }

            for (int j = 0; j < NPBUFTYPES; j++) {
                tmp_accesses[j] = p->accesses[j] >> (global_clock - p->local_clock);
            }

            if ((tmp_accesses[WRITE] < HOT_WRITE_THRESHOLD) && (tmp_accesses[READ] < HOT_READ_THRESHOLD)) {
                p->hot = false;
            }

            if (dram && (p == start_dram_page)) {
                start_dram_page = nullptr;
                need_cool_dram = false;
            }

            if (!dram && (p == start_nvm_page)) {
                start_nvm_page = nullptr;
                need_cool_nvm = false;
            }

            if (!p->hot) {
                current = p->next;
                page_list_remove_page(hot, p);
                enqueue(cold, p);
            }
            else {
                current = p;
            }
        }
        return current;
    }

    bool page_migrate(hemem_page *src, bool migrate_up, int app_id = 0) {
        // The core logic of finding a new page, swapping, and updating page tables
        // is now handled by the OS simulation (MimicOS).
        std::queue<hemem_page*> pages;  pages.push(src);
        std::queue<bool> migrate_ups;   migrate_ups.push(migrate_up);
        std::cout << "[Hemem] 0x"<< std::hex << pages.front()->vaddr << " : " << (migrate_up ? "↑" : "↓") << std::endl;
        bool success = Sim()->getMimicOS()->move_pages(pages, migrate_ups, app_id);
        return success;
    }

    Hemem::Hemem(PageTracer *p)
        :PageMigration(p),
        pages_hotness(),
        cold_ring(CAPACITY),
        hot_ring(CAPACITY),
        free_page_ring(CAPACITY),
        dram_hot_list("dram_hot"),
        nvm_hot_list("nvm_hot"),
        dram_cold_list("dram_cold"),
        nvm_cold_list("nvm_cold"),
        still_run(true)
    {
        this->setName("Hemem");
    }

    Hemem::~Hemem() {
        still_run = false;
        this->Hemem::stop();
    }


    void Hemem::scan() {
        bool perf_empty = false;
        while (still_run) {
            struct PerfSample page_sample = page_tracer->getPerfSample(&perf_empty);
            if (perf_empty) {
                usleep(PEBS_KSWAPD_INTERVAL);
                continue;
            }
            // std::cout << "[Hemem] perf addr : " << reinterpret_cast<void *>(page_sample.addr) << std::endl;
            UInt64 addr = page_sample.addr & pages_mask[0];  // base page
            auto it = pages_hotness.find(addr);
            if (it == pages_hotness.end()) {
                other_pages_cnt++;
                usleep(PEBS_KSWAPD_INTERVAL);
                continue;
                // return nullptr;
            }

            hemem_page *page = it->second;
            int access_index = -1;

            switch (page_sample.type) {
                case LLC_Miss_RD:
                    // std::cout << "read" << std::endl;
                    page->accesses[READ] += 1;
                    access_index = 0;
                    break;
                case LLC_Miss_ST:
                    // std::cout << "write" << std::endl;
                    page->accesses[WRITE] += 1;
                    access_index = 1;
                    break;
                default:
                    // We do not deal page fault in hemem
                    continue;
            }

            if (page->accesses[READ] >= HOT_READ_THRESHOLD || page->accesses[WRITE] >= HOT_READ_THRESHOLD) {
                // Make the page hot
                if (!page->hot || !page->ring_present) {
                    // std::cout << "[Hemem] "<<(void *)it->first<<" enter hot LRU" << std::endl;
                    page->ring_present = true;
                    hot_ring.push_back(page);
                }
            } else if (page->accesses[WRITE] < HOT_WRITE_THRESHOLD && page->accesses[READ] < HOT_READ_THRESHOLD) {
                // Make the page cold
                if (page->hot || !page->ring_present) {
                    // std::cout << "[Hemem] "<<(void *)it->first<<" enter cold LRU" << std::endl;
                    page->ring_present = true;
                    cold_ring.push_back(page);
                }
            }

            page->accesses[access_index] >>= (global_clock - page->local_clock);
            page->local_clock = global_clock;
            if (page->accesses[access_index] > PEBS_COOLING_THRESHOLD) {
                global_clock ++;
                need_cool_dram = true;
                need_cool_nvm = true;
            }
            hemem_pages_cnt++;
        }
        // return nullptr;
    }


    void Hemem::policy() {
        int num_ring_reqs, tries;
        hemem_page *p;
        hemem_page *cp;
        hemem_page *np;

        hemem_page *page = nullptr;
        hemem_page *cur_cool_in_dram = nullptr;
        hemem_page *cur_cool_in_nvm = nullptr;

        for (;still_run;) {

            // while (!free_page_ring.empty()) {
            //     fifo_list *list;
            //     page = reinterpret_cast<hemem_page *>(free_page_ring.front());
            //     if (page == nullptr) {
            //         continue;
            //     }
            //
            //     // remove page from ringbuffer
            //     free_page_ring.pop_front();
            //
            //     list = page->list;
            //     assert(list != nullptr);
            //     updateCurrentCoolPage(&cur_cool_in_dram, &cur_cool_in_nvm, page);
            //     page_list_remove_page(list, page);
            //     dynamic_cast<HememAllocator*>(Sim()->getMimicOS()->getPageFaultHandler()->getAllocator())->deallocate(page, page->in_dram, 0);
            // }

            num_ring_reqs = 0;
            while (!hot_ring.empty() && num_ring_reqs < HOT_RING_REQS_THRESHOLD) {
                page = hot_ring.front();
                hot_ring.pop_front();
                updateCurrentCoolPage(&cur_cool_in_dram, &cur_cool_in_nvm, page);
                assert(page != nullptr);
                page->ring_present = false;
                num_ring_reqs++;
                // std::cout << "[Hemem] hot page in dram : " << page->in_dram <<
                //     " dram_hot_list : " <<(page->list==&dram_hot_list) <<
                //     " dram_cold_list : " <<(page->list==&dram_cold_list) <<
                //     " nvm_hot_list : " <<(page->list==&nvm_hot_list) <<
                //     " nvm_cold_list : " <<(page->list==&nvm_cold_list)
                // << std::endl;
                makeHot(page);
            }

            while (!cold_ring.empty() && num_ring_reqs < COLD_RING_REQS_THRESHOLD) {
                page = cold_ring.front();
                cold_ring.pop_front();
                updateCurrentCoolPage(&cur_cool_in_dram, &cur_cool_in_nvm, page);
                page->ring_present = false;
                num_ring_reqs++;
                makeCold(page);
            }

            for (UInt64 migrated_bytes = 0; migrated_bytes < PEBS_KSWAPD_MIGRATE_RATE; ) {
                p = dequeue(&nvm_hot_list);
                if (p == nullptr) break;

                updateCurrentCoolPage(&cur_cool_in_dram, &cur_cool_in_nvm, p);

                if ((p->accesses[WRITE] < HOT_WRITE_THRESHOLD) && p->accesses[READ] < HOT_READ_THRESHOLD) {
                    p->hot = false;
                    // std::cout << "[Hemem] inserted " << page->prev << " in " << __LINE__ <<std::endl;
                    enqueue(&nvm_cold_list, p);
                    continue;
                }

                // Try to promote the page. OS will handle finding a free page or swapping.
                if (page_migrate(p, true, 0)) {
                    enqueue(&dram_hot_list, p);
                    migrated_bytes += pt_to_pagesize(p->pt);
                } else {
                    // Migration failed, maybe no free space. Try to make space.
                    // no free dram page, try to find a cold dram page to move down
                    cp = dequeue(&dram_cold_list);
                    if (cp == nullptr) {
                        // all dram pages are hot, so put it back in list we got it from
                        enqueue(&nvm_hot_list, p);
                        goto out;
                    }
                    
                    // Demote the cold page to NVM
                    if (page_migrate(cp, false, 0)) {
                        enqueue(&nvm_cold_list, cp);
                        // Now that a DRAM page is freed, put `p` back and retry in the next iteration.
                        enqueue(&nvm_hot_list, p);
                    } else {
                        // Demotion also failed. Put both pages back.
                        enqueue(&dram_cold_list, cp);
                        enqueue(&nvm_hot_list, p);
                    }
                }
            }
            cur_cool_in_dram = partial_cool_peek_and_move(&dram_hot_list, &dram_cold_list, true, cur_cool_in_dram);
            cur_cool_in_nvm = partial_cool_peek_and_move(&nvm_hot_list, &nvm_cold_list, false, cur_cool_in_nvm);
        }
        out:
            std::cout << "[Hemem] All pages resident in dram is hot" << std::endl;
            // return nullptr;
    }

    void Hemem::start() {
        still_run = true;
        std::cout << "[Hemem] Start daemons" << std::endl;
        scan_thread_handle = std::thread(&Hemem::scan, this);
        policy_thread_handle = std::thread(&Hemem::policy, this);
        // pthread_create(&scan_thread_handle, NULL, &scan_entry, NULL);
        // pthread_create(&policy_thread_handle, NULL, policy, NULL);
    }

    void Hemem::stop() {
        std::cout << "[Hemem] Stopping background threads..." << std::endl;
        still_run = false;
        if (scan_thread_handle.joinable()) {
            scan_thread_handle.join();
        }

        if (policy_thread_handle.joinable()) {
            policy_thread_handle.join();
        }

        std::cout << "[Hemem] Background threads stopped." << std::endl;
    }

    void Hemem::page_fault(UInt64 laddr, void* ptr) {
        hemem_page *page = static_cast<hemem_page *>(ptr);
        auto const& [it, inserted] = pages_hotness.insert({laddr, page});
        if (inserted) {
            // std::cout << "[Hemem] 0x" << std::hex << laddr << " Inserting into hotness map" << std::endl;
            // std::cout << "[Hemem] inserted " << page->prev << std::endl;
            if (page->in_dram)
                enqueue(&dram_cold_list, page);
            else
                enqueue(&nvm_cold_list, page);
        } else {
            std::cout << "[Hemem] Insert an existing page 0x" << std::hex << laddr << std::endl;
        }
    }


}