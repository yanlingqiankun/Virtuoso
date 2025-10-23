//
// Created by shado on 25-6-12.
//

#ifndef HEMEM_PAGE_FAULT_HANDLER_H
#define HEMEM_PAGE_FAULT_HANDLER_H

#include "page_fault_handler_base.h"
#include "physical_memory_allocator.h"
#include "fixed_types.h"
#include <ostream>


class HememPagingFultHandler : public PageFaultHandlerBase {
private:
    PhysicalMemoryAllocator *allocator;
    bool is_guest;
    String name;
    std::ofstream log_file;
    std::string log_file_name;
public:
    HememPagingFultHandler(PhysicalMemoryAllocator *allocator, bool is_guest);
    ~HememPagingFultHandler();

    void allocatePagetableFrames(UInt64 address, UInt64 core_id, UInt64 ppn, int page_size, int frame_number);
    void handlePageFault(UInt64 address, UInt64 core_id, int frames);
};



#endif //HEMEM_PAGE_FAULT_HANDLER_H
