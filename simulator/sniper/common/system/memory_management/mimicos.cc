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

using namespace std;

MimicOS::MimicOS(bool _is_guest) : m_page_fault_latency(NULL, 0), tlb_flush_latency(NULL, 0)
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

    if (Sim()->getCfg()->hasKey("perf_model/migration_enable")) {
        int sampling_frequency = Sim()->getCfg()->getInt("perf_model/" + mimicos_name + "/sampling_frequency");
        page_tracer = new PageTracer(sampling_frequency);
        page_migration_handler = MigrationFactory::createMigration(mimicos_name, page_tracer);
        if (page_migration_handler) {
            tlb_flush_latency = ComponentLatency(Sim()->getDvfsManager()->getGlobalDomain(),
                                                 Sim()->getCfg()->getInt(
                                                     "perf_model/" + mimicos_name + "/tlb_flush_latency"));
            std::cout << "[MimicOS] Page migration handler is " << page_migration_handler->getName() << std::endl;
            std::cout << "[MimicOS] TLB flush latency is " << tlb_flush_latency.getLatency().getNS() << "ns" << std::endl;
            page_migration_handler->start();
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

void MimicOS::flushTLB(int appid, UInt64 addr) {
    // std::cout << "[TLBSHOOTDOWN] flush [0x" << std::hex << addr << "] of appid [" << appid << "]" << std::endl;
    std::vector<UInt32> ret = Sim()->getCoreManager()->CoresFlushTLB(appid, addr);
    std::cout << "[TLBSHOOTDOWN] Address 0x" << std::hex << addr
          << "[ ";

    for (UInt32 core_id : ret) {
        std::cout << core_id << " ";
    }

    std::cout << " ]" << std::dec << std::endl;
}


void MimicOS::handle_page_fault(IntPtr address, IntPtr core_id, int frames)
{
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
