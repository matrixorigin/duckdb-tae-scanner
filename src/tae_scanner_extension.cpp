// Copyright 2024 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// DuckDB extension entry point for the TAE scanner.

#define DUCKDB_EXTENSION_MAIN

#include "tae_scanner.hpp"
#include "duckdb.hpp"
#include "duckdb/main/extension_util.hpp"

namespace tae {

class TAEScannerExtension : public duckdb::Extension {
public:
    void Load(duckdb::DuckDB &db) override {
        auto &instance = *db.instance;
        duckdb::ExtensionUtil::RegisterFunction(instance, GetTAEScanFunction());
    }

    std::string Name() override {
        return "tae_scanner";
    }

    std::string Version() const override {
        return "0.1.0";
    }
};

} // namespace tae

extern "C" {

DUCKDB_EXTENSION_API void tae_scanner_init(duckdb::DatabaseInstance &instance) {
    duckdb::ExtensionUtil::RegisterFunction(instance, tae::GetTAEScanFunction());
}

DUCKDB_EXTENSION_API const char *tae_scanner_version() {
    return "0.1.0";
}

} // extern "C"
