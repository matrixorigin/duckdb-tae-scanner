// Auto-generated extension header for DuckDB's static linking.
#pragma once

#include "duckdb.hpp"

namespace duckdb {

class TaeScannerExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
