// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// DuckDB extension entry point for the TAE scanner.

#define DUCKDB_EXTENSION_MAIN

#include "tae_scanner.hpp"
#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace tae {

class TAEScannerExtension : public duckdb::Extension {
public:
    void Load(duckdb::ExtensionLoader &loader) override {
        loader.RegisterFunction(GetTAEScanFunction());
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

DUCKDB_CPP_EXTENSION_ENTRY(tae_scanner, loader) {
    loader.RegisterFunction(tae::GetTAEScanFunction());
}

DUCKDB_EXTENSION_API const char *tae_scanner_version() {
    return "0.1.0";
}

} // extern "C"
