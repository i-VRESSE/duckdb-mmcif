cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME mmcif)

# DuckDB's extension distribution supports vcpkg. As such, dependencies can be added in ./vcpkg.json and then
# used in cmake with find_package. Feel free to remove or replace with other dependencies.
# Note that it should also be removed from vcpkg.json to prevent needlessly installing it..

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

# PROTOTYPE (throwaway): RCSB C++ mmcif core library, minimal read-only data-file set.
# Wired per .scratch/mmcif-extension/research/01-wire-rcsb-lib.md (flex/bison inside CMake).
find_package(FLEX REQUIRED)
find_package(BISON REQUIRED)

set(MMCIF_MOD "${CMAKE_CURRENT_SOURCE_DIR}/modules")
set(MMCIF_GEN "${CMAKE_CURRENT_BINARY_DIR}/mmcif-gen")
file(MAKE_DIRECTORY ${MMCIF_GEN})

# cpp-common (no deps)
set(MMCIF_COMMON
  ${MMCIF_MOD}/cpp-common/src/RcsbPlatform.C
  ${MMCIF_MOD}/cpp-common/src/RcsbFile.C
  ${MMCIF_MOD}/cpp-common/src/BlockIO.C
  ${MMCIF_MOD}/cpp-common/src/CifString.C
  ${MMCIF_MOD}/cpp-common/src/Serializer.C
  ${MMCIF_MOD}/cpp-common/src/GenString.C
  ${MMCIF_MOD}/cpp-common/src/GenCont.C
  ${MMCIF_MOD}/cpp-common/src/Exceptions.C
  ${MMCIF_MOD}/cpp-common/src/DataInfo.C
  ${MMCIF_MOD}/cpp-common/src/mapped_vector.C
  ${MMCIF_MOD}/cpp-common/src/mapped_ptr_vector.C)
# cpp-tables (dep: common)
set(MMCIF_TABLES
  ${MMCIF_MOD}/cpp-tables/src/ISTable.C
  ${MMCIF_MOD}/cpp-tables/src/ITTable.C
  ${MMCIF_MOD}/cpp-tables/src/TTable.C
  ${MMCIF_MOD}/cpp-tables/src/TableFile.C)
# cc-regex (C, no deps)
set(MMCIF_REGEX
  ${MMCIF_MOD}/cc-regex/src/regcomp.c
  ${MMCIF_MOD}/cc-regex/src/regexec.c
  ${MMCIF_MOD}/cc-regex/src/regerror.c
  ${MMCIF_MOD}/cc-regex/src/regfree.c)
# cpp-cif-file (dep: common, tables, regex)
set(MMCIF_CIF_FILE
  ${MMCIF_MOD}/cpp-cif-file/src/CifFile.C
  ${MMCIF_MOD}/cpp-cif-file/src/DicFile.C
  ${MMCIF_MOD}/cpp-cif-file/src/ParentChild.C
  ${MMCIF_MOD}/cpp-cif-file/src/CifParentChild.C
  ${MMCIF_MOD}/cpp-cif-file/src/CifDataInfo.C
  ${MMCIF_MOD}/cpp-cif-file/src/CifExcept.C)
# cpp-cif-parser (dep: common, tables) -- flex/bison generated C files live in ${MMCIF_GEN}
flex_target(mmcif_flex1 "${MMCIF_MOD}/cpp-cif-parser/src/CifScanner.l" "${MMCIF_GEN}/CifScanner.c" COMPILE_FLAGS "-Cfr -L -Pcifparser_")
flex_target(mmcif_flex2 "${MMCIF_MOD}/cpp-cif-parser/src/DICScanner.l" "${MMCIF_GEN}/DICScanner.c" COMPILE_FLAGS "-Cfr -L -Pdicparser_")
bison_target(mmcif_bison1 "${MMCIF_MOD}/cpp-cif-parser/src/CifParser.y" "${MMCIF_GEN}/CifParser.c" COMPILE_FLAGS "-d -v -l -p cifparser_")
bison_target(mmcif_bison2 "${MMCIF_MOD}/cpp-cif-parser/src/DICParser.y" "${MMCIF_GEN}/DICParser.c" COMPILE_FLAGS "-d -v -l -p dicparser_")
add_flex_bison_dependency(mmcif_flex1 mmcif_bison1)
add_flex_bison_dependency(mmcif_flex2 mmcif_bison2)

set(MMCIF_PARSER
  ${MMCIF_MOD}/cpp-cif-parser/src/CifFileReadDef.C
  ${MMCIF_GEN}/CifParser.c
  ${MMCIF_GEN}/CifScanner.c
  ${MMCIF_MOD}/cpp-cif-parser/src/CifScannerBase.C
  ${MMCIF_MOD}/cpp-cif-parser/src/CifParserBase.C
  ${MMCIF_GEN}/DICScanner.c
  ${MMCIF_MOD}/cpp-cif-parser/src/DICScannerBase.C
  ${MMCIF_GEN}/DICParser.c
  ${MMCIF_MOD}/cpp-cif-parser/src/DICParserBase.C)

# Build mmciflib as an OBJECT library and compile its objects into the
# extension targets directly. This avoids the install(EXPORT) export-set problem
# (mmciflib carries build-dir include paths, so exporting it fails).
add_library(mmciflib OBJECT ${MMCIF_COMMON} ${MMCIF_TABLES} ${MMCIF_REGEX} ${MMCIF_CIF_FILE} ${MMCIF_PARSER})
target_include_directories(mmciflib PUBLIC
  ${MMCIF_MOD}/cpp-common/include
  ${MMCIF_MOD}/cpp-tables/include
  ${MMCIF_MOD}/cc-regex/include
  ${MMCIF_MOD}/cpp-cif-file/include
  ${MMCIF_MOD}/cpp-cif-parser/include
  ${MMCIF_MOD}/cpp-common/src
  ${MMCIF_GEN})
# Research 01: cpp-common/src is required because cpp-tables headers do `#include "mapped_vector.C"`.
# Research 01: mmcif C++ is C++11; std::binary_function (GenString.h) hard-blocks a forced C++17 build.
set_target_properties(mmciflib PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
set_target_properties(mmciflib PROPERTIES POSITION_INDEPENDENT_CODE ON)

# mmcif_extension.cpp (an extension source) also needs the mmcif include
# dirs to compile, so add them globally (they apply to the extension targets too).
include_directories(
  ${MMCIF_MOD}/cpp-common/include
  ${MMCIF_MOD}/cpp-tables/include
  ${MMCIF_MOD}/cc-regex/include
  ${MMCIF_MOD}/cpp-cif-file/include
  ${MMCIF_MOD}/cpp-cif-parser/include
  ${MMCIF_MOD}/cpp-common/src
  ${MMCIF_GEN})

# Embed the checked-in gzip'd dictionary artifacts into mmcif_dict_data.hpp so the
# extension has no runtime data-file dependency (see .scratch/mmcif-extension/map.md).
# The header is generated into ${MMCIF_GEN}, which is on the include path above.
add_custom_command(
  OUTPUT ${MMCIF_GEN}/mmcif_dict_data.hpp
  COMMAND ${CMAKE_COMMAND} -E env python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_artifact.py
          ${MMCIF_GEN}/mmcif_dict_data.hpp
          ${CMAKE_CURRENT_SOURCE_DIR}/dict/mmcif_pdbx_v50_type_index.tsv.gz MMCIFF_TYPE_INDEX_GZ
          ${CMAKE_CURRENT_SOURCE_DIR}/dict/mmcif_pdbx_v50_relationships.tsv.gz MMCIFF_RELATIONSHIPS_GZ
  DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/dict/mmcif_pdbx_v50_type_index.tsv.gz
    ${CMAKE_CURRENT_SOURCE_DIR}/dict/mmcif_pdbx_v50_relationships.tsv.gz
  COMMENT "embedding mmcif dictionary artifacts"
  VERBATIM
)

# Make the extension targets build the generated dict-data header.
add_custom_target(mmciff_dict_data DEPENDS ${MMCIF_GEN}/mmcif_dict_data.hpp)

set(EXTENSION_SOURCES src/mmcif_extension.cpp src/mmcif_core.cpp src/mmcif_index.cpp $<TARGET_OBJECTS:mmciflib>)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

add_dependencies(${EXTENSION_NAME} mmciff_dict_data)
add_dependencies(${LOADABLE_EXTENSION_NAME} mmciff_dict_data)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
