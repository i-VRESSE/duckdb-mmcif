#define DUCKDB_EXTENSION_MAIN

#include "mmcif_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/config.hpp"

#include "mmcif_table_functions.hpp"
#include "mmcif_catalog.hpp"

namespace duckdb {

static void MmcifCoreLoad(ExtensionLoader &loader) {
	// Table functions (mmcif_scan, mmcif, columns variants, meta variants).
	MmcifRegisterTableFunctions(loader);

	// Storage extension (ATTACH 'file.cif').
	auto &db = loader.GetDatabaseInstance();
	MmcifRegisterStorageExtension(DBConfig::GetConfig(db));
}

void MmcifExtension::Load(ExtensionLoader &loader) {
	MmcifCoreLoad(loader);
}

std::string MmcifExtension::Name() {
	return "mmcif";
}

std::string MmcifExtension::Version() const {
#ifdef EXT_VERSION_MMCIF
	return EXT_VERSION_MMCIF;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(mmcif, loader) {
	duckdb::MmcifCoreLoad(loader);
}
}
