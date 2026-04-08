// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// DuckDB extension entry point for the TAE scanner.

#define DUCKDB_EXTENSION_MAIN

#include "tae_scanner.hpp"
#include "tae_scanner_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

// Static extension class used when linked into DuckDB.
namespace duckdb {

void TaeScannerExtension::Load(ExtensionLoader &loader) {
    loader.RegisterFunction(tae::GetTAEScanFunction());
}

std::string TaeScannerExtension::Name() {
    return "tae_scanner";
}

std::string TaeScannerExtension::Version() const {
    return "0.1.0";
}

} // namespace duckdb

// Dynamic extension entry point (LOAD 'tae_scanner.duckdb_extension').
extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(tae_scanner, loader) {
    loader.RegisterFunction(tae::GetTAEScanFunction());
}

DUCKDB_EXTENSION_API const char *tae_scanner_version() {
    return "0.1.0";
}

} // extern "C"
