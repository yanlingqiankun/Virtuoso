//
// Created by shado on 25-6-3.
//

#ifndef PAGE_MIGRATION_H
#define PAGE_MIGRATION_H

#include "page_tracer.h"
#include "fixed_types.h"
#include <iostream>
#include <map>

#define BASE_PAGE_SIZE (1UL << 12)
#define HUGE_PAGE_SIZE (1UL << 21)

#define BASE_PAGE_MASK (~(BASE_PAGE_SIZE - 1))
#define HUGE_PAGE_MASK (~(HUGE_PAGE_SIZE - 1))

typedef enum {
    LLC_Miss_RD = AccessType::READ,
    LLC_Miss_ST = AccessType::WRITE,
    PTE_access
}accType;

class PageMigration {
protected:
    String name;
    PageTracer *page_tracer;
    UInt64 pages_mask[2] = {(BASE_PAGE_MASK), (HUGE_PAGE_MASK)};

public:
    void setName(String name) {this->name = name; }
    String getName() {return this->name; }
    PageMigration(PageTracer *pt) {
        if (pt != nullptr)
            page_tracer = pt;
        else
            std::cout << "[Page Migration] get a null page tracer" << std::endl;
    }
    virtual void page_fault(UInt64 laddr, void *ptr){}
    virtual void start() = 0;
    virtual void stop() = 0;
};

#endif //PAGE_MIGRATION_H
