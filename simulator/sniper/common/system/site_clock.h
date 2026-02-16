#pragma once

#include "fixed_types.h"
#include <atomic>
#include <vector>

/**
 * @brief Global logical clock for SITE (Self-Invalidating TLB Entries).
 *
 * The clock is based on the number of main memory (DRAM) accesses,
 * NOT on physical time or clock cycles.
 *
 * The memory controller increments the global counter on each DRAM access.
 * Periodically (every BROADCAST_INTERVAL accesses), the global time is
 * "broadcast" to per-core local registers.
 */
class SiteLogicalClock
{
public:
    static const UInt32 BROADCAST_INTERVAL = 100; // Broadcast every 100 DRAM accesses
    static const UInt32 DEFAULT_INITIAL_LEASE = 200; // Default initial lease length

    static SiteLogicalClock* getInstance()
    {
        static SiteLogicalClock instance;
        return &instance;
    }

    /**
     * @brief Increment the global access counter. Called by DRAM controller on each access.
     */
    void incrementAccessCounter()
    {
        UInt32 old_val = m_global_access_counter.fetch_add(1, std::memory_order_relaxed);
        // Broadcast periodically
        if ((old_val + 1) % BROADCAST_INTERVAL == 0) {
            broadcastTime();
        }
    }

    /**
     * @brief Get the current global logical time.
     */
    UInt32 getGlobalTime() const
    {
        return m_global_access_counter.load(std::memory_order_relaxed);
    }

    /**
     * @brief Get the local logical time for a specific core.
     * @param core_id The core ID.
     */
    UInt32 getCoreLocalTime(core_id_t core_id) const
    {
        if (core_id < 0 || (size_t)core_id >= m_per_core_time.size())
            return m_global_access_counter.load(std::memory_order_relaxed);
        return m_per_core_time[core_id];
    }

    /**
     * @brief Initialize per-core time registers. Call once during setup.
     * @param num_cores Number of cores in the system.
     */
    void init(int num_cores)
    {
        m_per_core_time.resize(num_cores, 0);
        m_global_access_counter.store(0, std::memory_order_relaxed);
    }

private:
    SiteLogicalClock() : m_global_access_counter(0) {}

    /**
     * @brief Broadcast the current global time to all per-core registers.
     */
    void broadcastTime()
    {
        UInt32 current = m_global_access_counter.load(std::memory_order_relaxed);
        for (size_t i = 0; i < m_per_core_time.size(); i++) {
            m_per_core_time[i] = current;
        }
    }

    std::atomic<UInt32> m_global_access_counter;
    std::vector<UInt32> m_per_core_time; // Per-core local time registers
};
