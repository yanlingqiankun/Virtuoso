//
// Created by shado on 25-6-3.
//
#pragma once
#ifndef MIGRATION_FACTORY_H
#define MIGRATION_FACTORY_H

#include <cstddef>
#include <iostream>

#include "simulator.h"
#include "page_migration.h"

class MigrationFactory {
public:
    static PageMigration *createMigration(String mimicos_name, PageTracer *p) {
        // bool migration_open = Sim()->getCfg()->getBool("per_model/"+mimicos_name+"/migration");
        // if (!migration_open) {
        //     return NULL;
        // }
        String migration_type = Sim()->getCfg()->getString("perf_model/"+mimicos_name+"/migration_type");
        std::cout << "[Page migration] migration type is "<< migration_type << std::endl;
        if (migration_type == "hemem") {
            return new Hemem::Hemem(p);
        } else if (migration_type == "memtis") {
            return NULL;
        } else if (migration_type == "nomad") {
            return NULL;
        } else {
            std::cout<< "[Migration] Unkown migration type";
            return NULL;
        }

    }
};

#endif //MIGRATION_FACTORY_H
