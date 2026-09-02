PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=mmcif
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

.PHONY: test_cpp

# C++ catch unit tests for the mmcif core (parser / index / write store / writer).
# The target is EXCLUDE_FROM_ALL, so it is not part of `make release`; build it
# here and run it. Depends on `release` only to guarantee build/release is
# configured.
test_cpp: release
	cmake --build build/release --target mmcif_catch_tests
	./build/release/test/cpp/mmcif_catch_tests