//
// Created by shado on 25-6-3.
//

#ifndef PAGE_TRACER_H
#define PAGE_TRACER_H
#include <boost/circular_buffer.hpp>
#include "fixed_types.h"

#define PERF_SAMPLE_CAPACITY (1024*1024)

using namespace std;
typedef enum {
    READ, //Core::mem_op_t::READ
    WRITE,
}AccessType;
struct PerfSample {
    AccessType type;
    IntPtr ip;
    UInt32 app_id;
    UInt64 addr;
};

class PageTracer {
private:
    boost::circular_buffer<PerfSample> perf_buffer;
    int frequency;
    int counter;

public:

    explicit PageTracer(int frequency):perf_buffer(PERF_SAMPLE_CAPACITY), frequency(frequency), counter(0) {};
    PageTracer():perf_buffer(0), frequency(0), counter(0){};
    ~PageTracer()= default;

    bool record(UInt64 addr, AccessType type, UInt32 id, IntPtr ip);
    PerfSample getPerfSample(bool *is_empty);
};

#endif //PAGE_TRACER_H
