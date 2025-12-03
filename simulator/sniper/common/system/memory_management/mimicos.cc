#include "mimicos.h"
#include "config.hpp"
#include "page_fault_handler_base.h"
#include "allocator_factory.h"
#include "pagetable_factory.h"
#include "handler_factory.h"
#include "rangetable_factory.h"
#include "dvfs_manager.h"
#include <string>
#include "page_migration/migration_factory.h"
#include "page_migration/hemem.h"
#include "core_manager.h"
#include "hemem_allocator.h"

using namespace std;

MimicOS::MimicOS(bool _is_guest) : m_page_fault_latency(NULL, 0), tlb_flush_latency(NULL, 0), 
                                ipi_initiate_latency(NULL, 0), ipi_handle_latency(NULL, 0)
{

    is_guest = _is_guest;
    if (is_guest)
    {
        mimicos_name = "mimicos_guest";
        std::cout << "[MimicOS] Guest OS is enabled" << std::endl;
    }
    else
    {
        mimicos_name = "mimicos_host";
        std::cout << "[MimicOS] Host OS is enabled" << std::endl;
    }

    page_table_type = Sim()->getCfg()->getString("perf_model/" + mimicos_name + "/page_table_type");
    page_table_name = Sim()->getCfg()->getString("perf_model/" + mimicos_name + "/page_table_name");

    range_table_type = Sim()->getCfg()->getString("perf_model/" + mimicos_name + "/range_table_type");
    range_table_name = Sim()->getCfg()->getString("perf_model/" + mimicos_name + "/range_table_name");

    m_memory_allocator = AllocatorFactory::createAllocator(mimicos_name);
    m_memory_allocator->fragment_memory();

    page_fault_handler = HandlerFactory::createHandler(Sim()->getCfg()->getString("perf_model/"+mimicos_name+"/page_fault_handler"), m_memory_allocator, mimicos_name, is_guest);
    m_page_fault_latency = ComponentLatency(Sim()->getDvfsManager()->getGlobalDomain(), Sim()->getCfg()->getInt("perf_model/"+mimicos_name+"/page_fault_latency"));

    number_of_page_sizes = Sim()->getCfg()->getInt("perf_model/" + mimicos_name + "/number_of_page_sizes");
    page_size_list = new int[number_of_page_sizes];

    for (int i = 0; i < number_of_page_sizes; i++)
    {
        page_size_list[i] = Sim()->getCfg()->getIntArray("perf_model/" + mimicos_name + "/page_size_list", i);
    }

    std::cout << "[MimicOS] Page fault latency is " << m_page_fault_latency.getLatency().getNS() << " ns" << std::endl;

    if (Sim()->getCfg()->hasKey("migration/migration_enable")) {
        page_migration_handler = MigrationFactory::createMigration(mimicos_name);
        if (page_migration_handler) {
            TLB_SHOOT_DOWN_SIZE = Sim()->getCfg()->getInt("migration/tlb_shootdown_size");
            page_migration_handler->setBatchSize(TLB_SHOOT_DOWN_SIZE);
            tlb_flush_latency = ComponentLatency(Sim()->getDvfsManager()->getGlobalDomain(),
                                                 Sim()->getCfg()->getInt(
                                                     "migration/tlb_flush_latency"));
            ipi_initiate_latency = ComponentLatency(Sim()->getDvfsManager()->getGlobalDomain(),
                                                 Sim()->getCfg()->getInt(
                                                     "migration/ipi_initiate_latency"));
            ipi_handle_latency = ComponentLatency(Sim()->getDvfsManager()->getGlobalDomain(),
                                                 Sim()->getCfg()->getInt(
                                                     "migration/ipi_handle_latency"));
            std::cout << "[MimicOS] Page migration handler is " << page_migration_handler->getName() << std::endl;
            std::cout << "[MimicOS] TLB flush latency is " << tlb_flush_latency.getLatency().getNS() << "ns" << std::endl;
        } else {
            std::cout << "[MimicOS] Page migration handler disabled" << std::endl;
        }
    } else {
        std::cout << "[MimicOS] Page migration disabled" << std::endl;
    }
}

MimicOS::~MimicOS()
{
    delete m_memory_allocator;
}

/**
 * @brief (Modified) Receives a pre-filled array of addresses (one batch) and issues a TLB shootdown request.
 *
 * @param app_id The application ID.
 * @param page_batch A fixed-size array containing the virtual addresses to be flushed.
 * The caller (e.g., move_pages) is responsible for ensuring this array
 * is correctly padded (filled with 0s if not full).
 * @return core_id_t The ID of the core that issued this request.
 */
core_id_t MimicOS::flushTLB(int app_id, array<IntPtr, TLB_SHOOT_DOWN_MAX_SIZE> page_batch, int page_num)
{
    CoreManager *core_manager = Sim()->getCoreManager();
    UInt32 total_cores = Sim()->getConfig()->getTotalCores();

    // Randomly select one core to issue the TLB shootdown
    core_id_t issuer_core_id = (rand() % total_cores);
    Core* issuer_core = core_manager->getCoreFromID(issuer_core_id);

    // Assume page_batch has been prepared (padded) by move_pages
    // Send this batch of TLB Shootdown requests
    issuer_core->enqueueTLBShootdownRequest(page_batch, issuer_core_id, app_id, page_num);

    return issuer_core_id;
}

/**
 * @brief (Modified) Migrates a batch of pages, handling batching internally
 * according to TLB_SHOOT_DOWN_SIZE.
 *
 * @param src_pages_queue Queue containing pointers to the source pages to be migrated.
 * @param migrate_up_queue Queue containing the migration direction for each corresponding source page
 * (true=up, false=down). This queue MUST be the same size as src_pages_queue.
 * @param app_id The application ID.
 * @return true if all pages were successfully migrated, false otherwise.
 */
bool MimicOS::move_pages(std::queue<Hemem::hemem_page*> src_pages_queue,
                         std::queue<bool> migrate_up_queue,
                         int app_id)
{
    // cout << __func__ << " start : 0x" << src_pages_queue.front()->vaddr << endl;
    HememAllocator* allocator = dynamic_cast<HememAllocator*>(getMemoryAllocator());
    if (!allocator) {
        std::cerr << "[MimicOS] Error: HememAllocator not found." << std::endl;
        return false;
    }

    ParametricDramDirectoryMSI::PageTable* pt = getPageTable(app_id);
    if (!pt) {
        std::cerr << "[MimicOS] Error: PageTable not found for app_id " << app_id << std::endl;
        return false;
    }

    // --- Robustness Check ---
    assert(src_pages_queue.size() == migrate_up_queue.size() && "Source pages and migration directions queue sizes must match!");

    bool all_succeeded = true;
    core_id_t issuer_core_id = 0; // Will be set by the first flushTLB call

    // --- Batch containers (for remapping logic) ---
    std::queue<Hemem::hemem_page*> batch_pages_to_migrate; // Stores source pages (for steps 4/5/6)
    std::queue<Hemem::hemem_page*> batch_dst_pages_alloced;  // Stores allocated destination pages (for steps 4/5/6)
    std::queue<bool> batch_directions_for_migration; // Stores directions (for steps 4/5/6)

    // --- Arrays for flushTLB and DMA_map ---
    std::array<IntPtr, TLB_SHOOT_DOWN_MAX_SIZE> batch_vaddrs_array{};
    std::array<IntPtr, TLB_SHOOT_DOWN_MAX_SIZE> batch_new_phy_addrs_array{};
    int batch_count = 0;

    // --- Step 1: Iterate, allocate, and process in batches ---
    while (!src_pages_queue.empty())
    {
        Hemem::hemem_page* src_page = src_pages_queue.front();
        bool current_migrate_up = migrate_up_queue.front();
        src_pages_queue.pop();
        migrate_up_queue.pop();

        if (!src_page || src_page->vaddr == 0) {
             all_succeeded = false;
             continue; // Skip invalid source pages
        }

        // --- Step 1a: Pre-allocate destination page ---
        Hemem::hemem_page* dst_page = allocator->getAFreePage(current_migrate_up);

        if (dst_page == nullptr) {
            std::cout << "[MimicOS] No free page in " << (current_migrate_up ? "DRAM" : "NVM")
                      << " for migration. Page 0x" << std::hex << src_page->vaddr << " failed." << std::endl;
            all_succeeded = false;
        } else {
            // Add to batch queues (for steps 4/5/6)
            batch_pages_to_migrate.push(src_page);
            batch_dst_pages_alloced.push(dst_page);
            batch_directions_for_migration.push(current_migrate_up);

            // Fill info into arrays (for steps 2, 3, 7)
            batch_vaddrs_array[batch_count] = src_page->vaddr;
            batch_new_phy_addrs_array[batch_count] = dst_page->phy_addr; // This is the new physical address the vaddr will use
            batch_count++;
        }

        // --- Step 1b: Process the batch if it's full OR it's the last page ---
        if ( (batch_count == TLB_SHOOT_DOWN_SIZE) || (src_pages_queue.empty() && batch_count > 0) )
        {
            // --- Padding ---
            IntPtr batch_key = batch_vaddrs_array[0]; // Use the first vaddr in the batch as the map key

            if (batch_count < TLB_SHOOT_DOWN_SIZE) {
                // If this is the last batch and it's not full, pad with 0s
                for (int i = batch_count; i < TLB_SHOOT_DOWN_SIZE; ++i) {
                    batch_vaddrs_array[i] = 0; // Mark as invalid
                    batch_new_phy_addrs_array[i] = 0; // Mark as invalid
                }
            }

            // --- Step 2: Mark all PTEs as "migrating" (Invalidate PTE) ---
            for (int i = 0; i < batch_count; ++i) {
                // (We only mark the actually valid addresses)
                // cout << "set page_moving : 0x" << hex << batch_vaddrs_array[i] << endl;
                pt->page_moving(batch_vaddrs_array[i]);
            }

            // --- Step 3: Flush Cache & TLB (Flush Cache & TLB Shootdown) [Blocking] ---
            // Pass the pre-filled array directly
            // cout << "flush tlb : 0x" << batch_vaddrs_array[0] << endl;
            issuer_core_id = flushTLB(app_id, batch_vaddrs_array, batch_count);

            // --- At this point, TLBs for this batch are clean ---

            // --- Steps 4, 5, 6: Swap Metadata (Remap), Free Old Frames in page allocator---
            while (!batch_pages_to_migrate.empty()) {
                Hemem::hemem_page* src_page_batch = batch_pages_to_migrate.front();
                Hemem::hemem_page* dst_page_batch = batch_dst_pages_alloced.front();
                bool current_migrate_up_batch = batch_directions_for_migration.front();
                batch_pages_to_migrate.pop();
                batch_dst_pages_alloced.pop();
                batch_directions_for_migration.pop();

                // --- Step 4: Swap Physical Addresses & Metadata (Remap) ---
                // cout << "exchange phy_addr : 0x" << src_page_batch->phy_addr << " to 0x" << dst_page_batch->phy_addr << endl;
                UInt64 temp_phy_addr = src_page_batch->phy_addr;
                src_page_batch->phy_addr = dst_page_batch->phy_addr; // vaddr gets the new paddr
                dst_page_batch->phy_addr = temp_phy_addr;            // old paddr is transferred to the dst_page struct

                src_page_batch->in_dram = current_migrate_up_batch;

                // --- Step 5: Free the old physical page frame (now tied to dst_page struct) ---
                dst_page_batch->vaddr = 0;
                dst_page_batch->present = false;
                // cout << "release page : 0x" << dst_page_batch->phy_addr << endl;
                allocator->deallocate(dst_page_batch, !current_migrate_up_batch, 0); // Return to the *source* tier's free list
            }

            // --- Step 7: Record in DMA_map ---
            DMA_map[batch_key] = std::make_pair(batch_vaddrs_array, batch_new_phy_addrs_array);

            // --- Reset counter for the next batch ---
            batch_count = 0;
            // Queues (batch_pages_to_migrate, etc.) were cleared in steps 4/5/6
            // Arrays (batch_vaddrs_array, etc.) will be overwritten in the next iteration
        }
    } // End while(!src_pages_queue.empty())
    // cout << __func__ << "end " << endl;
    return all_succeeded;
}

/**
 * @brief Wrapper for move_pages to handle raw virtual addresses from syscalls.
 * It translates virtual addresses to hemem_page pointers and calls the internal move_pages.
 * * @param src_pages_address_queue Queue of virtual addresses to migrate.
 * @param migrate_up_queue Queue of directions (true = up/DRAM, false = down/NVM).
 * @param app_id Application ID.
 * @return true if successful, false otherwise.
 */
bool MimicOS::move_pages_syscall(std::queue<IntPtr> src_pages_address_queue,
                                 std::queue<bool> migrate_up_queue,
                                 int app_id)
{
    // 1. Basic Validation
    if (src_pages_address_queue.size() != migrate_up_queue.size()) {
        std::cerr << "[MimicOS] Error: move_pages_syscall address queue and direction queue size mismatch." << std::endl;
        return false;
    }

    if (!page_migration_handler) {
        std::cerr << "[MimicOS] Error: No page migration handler attached. Cannot lookup page metadata." << std::endl;
        return false;
    }

    // 2. Containers for converted data
    std::queue<Hemem::hemem_page*> internal_pages_queue;
    std::queue<bool> valid_directions_queue;

    int total_requested = src_pages_address_queue.size();
    int valid_pages = 0;

    // 3. Translation Loop (Vaddr -> hemem_page*)
    while (!src_pages_address_queue.empty()) {
        IntPtr vaddr = src_pages_address_queue.front();
        bool direction = migrate_up_queue.front();

        src_pages_address_queue.pop();
        migrate_up_queue.pop();

        // 3.1 Align address to page boundary (robustness)
        // Assuming base page size (4KB) for lookup key
        IntPtr aligned_vaddr = vaddr & (~(4095ULL));

        // 3.2 Lookup the internal page structure
        // This relies on the getPage() interface added to PageMigration
        Hemem::hemem_page* page = dynamic_cast<Hemem::Memtis*>(page_migration_handler)->getPage(aligned_vaddr);

        if (page != nullptr) {
            internal_pages_queue.push(page);
            valid_directions_queue.push(direction);
            valid_pages++;
        } else {
            // Option: Log warning for pages not found (e.g., not faulted in yet)
            // std::cout << "[MimicOS] Warning: move_pages_syscall could not find metadata for vaddr 0x"
            //           << std::hex << vaddr << std::dec << ". Skipping." << std::endl;
        }
    }

    // 4. Call the internal implementation
    if (internal_pages_queue.empty()) {
        // If no valid pages were found, return true (nothing to do is technically a success)
        // or false depending on strictness requirements.
        return true;
    }

    // std::cout << "[MimicOS] move_pages_syscall: Translating " << valid_pages
    //           << " / " << total_requested << " requests to internal migration." << std::endl;

    return move_pages(internal_pages_queue, valid_directions_queue, app_id);
}

void MimicOS::DMA_migrate(IntPtr move_id, subsecond_time_t finish_time, int app_id) {

    // 1. Find the batch in the map
    auto it = DMA_map.find(move_id);

    // 2. Ensure the batch was found
    if (it == DMA_map.end()) {
        // Not found, just return
        return;
    }

    // 3. Get the Page Table
    ParametricDramDirectoryMSI::PageTable* pt = getPageTable(app_id);

    // 4. Get both address arrays from the map (virtual and new physical)
    const auto& vaddrs_array = it->second.first;
    const auto& new_phy_addrs_array = it->second.second;

    // 5. Iterate over all pages in the batch
    for (int i = 0; i < TLB_SHOOT_DOWN_SIZE; ++i) {

        IntPtr vaddr = vaddrs_array[i];

        // If vaddr is 0, it's a padding entry, so the batch is done
        if (vaddr == 0) {
            break;
        }

        // Get the corresponding new physical address
        IntPtr new_paddr = new_phy_addrs_array[i];

        // 6. Update the Page Table (PTE), pointing the vaddr to the new_paddr
        pt->DMA_move_page(vaddr, finish_time);
    }

    // 7. Processing is complete, remove this entry from the map
    DMA_map.erase(it);
}


void MimicOS::handle_page_fault(IntPtr address, IntPtr core_id, int frames)
{
    // todo: app_id = 0 only support multi-threaded simulation
    ParametricDramDirectoryMSI::PageTable *pt = getPageTable(0);
    std::unique_lock<std::shared_mutex> write_mutex(pt->get_lock_for_page(address));
    if (pt->check_page_exist(address)) {
        // cout << "PTE of 0x" << address << " has been created" << endl;
        return;
    }
    page_fault_handler->handlePageFault(address, core_id, frames);
}

void MimicOS::createApplication(int app_id)
{
    if (page_tables.find(app_id) != page_tables.end())
    {
        std::cout << "[MimicOS] Application " << app_id << " already exists" << std::endl;
        return;
    }

    std::cout << "[MimicOS] Creating application " << app_id << " with page table type " << page_table_type << " and name " << page_table_name << std::endl;

    // Create a new page table for the application
    ParametricDramDirectoryMSI::PageTable *page_table = ParametricDramDirectoryMSI::PageTableFactory::createPageTable(page_table_type, page_table_name, app_id, is_guest);
    page_tables[app_id] = page_table;

    ParametricDramDirectoryMSI::RangeTable *range_table = ParametricDramDirectoryMSI::RangeTableFactory::createRangeTable(range_table_type, range_table_name, app_id);
    range_tables[app_id] = range_table;

    std::cout << "[MimicOS] Parsing provided VMAs for application " << app_id << std::endl;

    // Parse the provided VMAs from the file: /path/to/input/trace/trace.vma
    // Convert the app_id to a GNU String



    String app_id_str = to_string(app_id).c_str();
    
    if (!Sim()->getCfg()->hasKey("traceinput/thread_" + app_id_str))
    {
        std::cout << "[MimicOS] No VMA file provided for application " << app_id << std::endl;
        return;
    }

    String trace_file = Sim()->getCfg()->getString("traceinput/thread_" + app_id_str);
   
    std::ifstream trace((trace_file + ".vma").c_str());

    if (!trace.is_open())
    {
        std::cout << "[MimicOS] Error opening file " << trace_file << ".vma" << std::endl;
        return;
    }

    std::vector<VMA> vmas;

    std::string line;
    while (std::getline(trace, line))
    {
        std::stringstream ss(line);
        std::string startStr, endStr;

        if (std::getline(ss, startStr, '-') && std::getline(ss, endStr))
        {
            try
            {
                IntPtr start = std::stoull(startStr, nullptr, 16);
                IntPtr end = std::stoull(endStr, nullptr, 16);

                VMA vma(start, end);
                vmas.push_back(vma);
            }
            catch (const std::invalid_argument &e)
            {
                std::cerr << "Error: Invalid VMA format in line: " << line << std::endl;
            }
            catch (const std::out_of_range &e)
            {
                std::cerr << "Error: VMA out of range in line: " << line << std::endl;
            }
        }
        else
        {
            std::cerr << "Error: Invalid line format: " << line << std::endl;
        }
    }

    vm_areas[app_id] = vmas;
    std::cout << "[MimicOS] VMAs for application " << app_id << " have been parsed" << std::endl;

// Print the VMAs for the application
#ifdef DEBUG
    for (auto vma : vmas)
    {
        vma.printVMA();
    }
#endif
    return;
}
