
#pragma once
#include "physical_memory_allocator.h"
#include <iostream>
#include <fstream>
#include "page_migration/hemem.h"

using namespace std;
class PageFaultHandlerBase
{
    private:
        PhysicalMemoryAllocator *getAllocator() {return this->allocator;}
    protected:
        PhysicalMemoryAllocator *allocator;

    public:
        friend class Hemem::Hemem;

        PageFaultHandlerBase(PhysicalMemoryAllocator *allocator){
            this->allocator = allocator;
        }
        
        ~PageFaultHandlerBase(){};

       virtual void allocatePagetableFrames(UInt64 address, UInt64 core_id, UInt64 ppn, int page_size, int frame_number) = 0;
       virtual void handlePageFault(UInt64 address, UInt64 core_id, int frames) = 0;
};