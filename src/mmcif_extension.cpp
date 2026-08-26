#define DUCKDB_EXTENSION_MAIN

#include "mmcif_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

void MmcifCoreLoad(ExtensionLoader &loader);

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
