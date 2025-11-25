//
// Created by shado on 25-6-5.
//

#ifndef HEMEM_H
#define HEMEM_H

#include </usr/include/semaphore.h>
#include <thread>
#include "page_migration.h"
#include "fixed_types.h"
#include <boost/circular_buffer.hpp>

namespace Hemem {

    struct fifo_list_t;
    struct hemem_page_t;

    typedef enum{
        HUGEP = 0,
        BASEP = 1,
        NPAGETYPES
    }pageTypes;

    typedef enum{
        READ = 0,
        WRITE = 1,
        NPBUFTYPES
    }pBufType;

    struct hemem_page_t{
        UInt64 vaddr;
        bool in_dram;
        pageTypes pt;
        bool migrating;
        bool present;
        bool written;
        bool hot;
        UInt64 naccesses;
        UInt64 migrations_up, migrations_down;
        bool ring_present;
        UInt64 local_clock;
        UInt64 accesses[NPBUFTYPES];
        UInt64 tot_accesses[NPBUFTYPES];

        UInt64 phy_addr;
        hemem_page_t *next, *prev;
        fifo_list_t *list;

        hemem_page_t() :accesses{},tot_accesses{}{
            vaddr = 0;
            in_dram = true;
            pt = BASEP;
            migrating = false;
            present = false;
            written = false;
            hot = false;
            naccesses = 0;
            migrations_up = migrations_down = 0;
            ring_present = false;
            local_clock = 0;
            phy_addr = 0;
            next = prev = nullptr;
            list = nullptr;
        }
        ~hemem_page_t(){}
    };

    struct fifo_list_t{
        hemem_page_t *first, *last;
        UInt64 numentries;
        String name;
        pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

        bool empty() {
            return !numentries;
        }

        fifo_list_t(String name):name(name){
            first = nullptr;
            last = nullptr;
            numentries = 0;
        }

        void lock() {
            pthread_mutex_lock(&mutex);
        }

        void unlock() {
            pthread_mutex_unlock(&mutex);
        }
    };

    typedef struct hemem_page_t hemem_page;
    typedef struct fifo_list_t fifo_list;
    void enqueue(fifo_list *queue, hemem_page *entry);

    hemem_page *dequeue(fifo_list *queue);
    void page_list_remove_page(fifo_list *list, hemem_page *page);

    class Hemem : public PageMigration{
    private:
        std::map<UInt64, hemem_page*> pages_hotness;
        boost::circular_buffer<hemem_page*> cold_ring, hot_ring, free_page_ring;
        fifo_list dram_hot_list, nvm_hot_list, dram_cold_list, nvm_cold_list;
        bool still_run;
        std::thread scan_thread_handle, policy_thread_handle;
        void makeHot(hemem_page *page);
        void makeCold(hemem_page *page);
        void scan();
        void policy();

    public:
        Hemem();
        ~Hemem();
        void start();
        void stop();
        void page_fault(UInt64 laddr, void *ptr) override;
    };
    pageTypes pagesize_to_pt(int page_size);
}

#endif //HEMEM_H
