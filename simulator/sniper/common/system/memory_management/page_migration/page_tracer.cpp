//
// Created by shado on 25-6-3.
//

#include "page_tracer.h"
#include <iostream>

bool PageTracer::record(UInt64 addr, AccessType type, UInt32 id, UInt64 ip) {
    if (__glibc_likely(counter != frequency)) {
        ++counter;
        return false;
    }
    // std::cout << "[PageTracer] addr = " << std::hex << "0x" << addr << std::endl;
    PerfSample perf_data = {type, ip, id, addr};
    perf_buffer.push_back(perf_data);
    counter = 0;
    return perf_buffer.full();
}

PerfSample PageTracer::getPerfSample(bool *is_empty) {
    PerfSample ret;
    if (perf_buffer.empty()) {
        *is_empty = true;
        ret = {};
    } else {
        *is_empty = false;
        ret = perf_buffer.front();
        perf_buffer.pop_front();
    }
    return ret;
}
