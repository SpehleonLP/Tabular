# Tabular's own build: the library as an object library, and its tests as a second one
# when TABULAR_TESTS is on. Authoritative source lists: add and remove files HERE.
#
# Added with add_subdirectory by the host (the engine's CMakeLists.txt), which links the
# objects into its executable. Tabular is not yet self-contained: it includes
# Spehleon/lib's Errors, Support and Yaml, ThirdParty's cityhash and
# tl::expected, and the tests include gtest and the engine's allocation counter
# (src/Support/allocationtestingsupport.h). The host supplies those include roots and
# links their objects; scripts/test-tabular-budgets.sh lists what a standalone link needs.
# Opened on its own (an IDE project), it compiles against those roots in the enclosing
# engine tree, but links nothing.
cmake_minimum_required(VERSION 3.28)
project(Tabular LANGUAGES CXX)

option(TABULAR_TESTS "Build Tabular's gtests as the tabular_tests object library" ${PROJECT_IS_TOP_LEVEL})

if(PROJECT_IS_TOP_LEVEL)
	set(CMAKE_CXX_EXTENSIONS ON)
	cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH _spehleon)
	cmake_path(GET _spehleon PARENT_PATH _engine)
	# ${_engine}/include holds helgrind.hpp, which the engine's build and its .clangd
	# force-include into every file; without it clangd cannot parse anything here.
	include_directories(
		${_spehleon}/lib
		${_engine}/include
		${_engine}/src
		${_engine}/ThirdParty
	)
endif()

set(TABULAR_SOURCES
	src/relation.cpp
	src/owner_handle.cpp
	src/pick.cpp
	src/layout.cpp
	src/version_checks.cpp
	src/machine.cpp
	src/instance.cpp
	src/grid.cpp
	src/schema_yaml.cpp
	src/command_text.cpp
	src/csv_text.cpp
	src/rows_csv.cpp
	src/pivot_csv.cpp
	src/grid_csv.cpp
)

# Headers are listed so IDEs show them in the project; the build does not need them.
set(TABULAR_HEADERS
	include/tabular/draw.h
	include/tabular/grid.h
	include/tabular/instance.h
	include/tabular/machine.h
	include/tabular/owner_handle.h
	include/tabular/pick.h
	include/tabular/relation.h
	include/tabular/rows_csv.h
	include/tabular/schema_yaml.h
	src/csv_cells.h
	src/command_text.h
	src/csv_text.h
	src/float_text.h
	src/layout.h
	src/machine_internal.h
	src/pick_internal.h
	src/relation_internal.h
	src/table_layout.h
	src/version_checks.h
)

set(TABULAR_TEST_HEADERS
	test/probe_closure.h
	bench/budgets.h
)

set(TABULAR_TEST_SOURCES
	test/relation_schema_gtest.cpp
	test/relation_store_gtest.cpp
	test/relation_find_gtest.cpp
	test/relation_load_gtest.cpp
	test/relation_load_conditions_gtest.cpp
	test/relation_columns_gtest.cpp
	test/schema_yaml_gtest.cpp
	test/rows_csv_gtest.cpp
	test/csv_hash_gtest.cpp
	test/csv_quoting_gtest.cpp
	test/pick_schema_gtest.cpp
	test/pick_gtest.cpp
	test/coverage_gtest.cpp
	test/pivot_csv_gtest.cpp
	test/grid_gtest.cpp
	test/owner_handle_gtest.cpp
	test/generation_gtest.cpp
	test/suffix_gtest.cpp
	test/draw_gtest.cpp
	test/pick_weighted_gtest.cpp
	test/key_domain_gtest.cpp
	test/display_selection_gtest.cpp
	test/machine_gtest.cpp
	test/instance_gtest.cpp
	test/effects_gtest.cpp
	test/edge_gtest.cpp
	test/reload_gtest.cpp
	test/evidence_gtest.cpp
	test/budget_gtest.cpp
	bench/budgets.cpp
)

add_library(tabular OBJECT ${TABULAR_SOURCES} ${TABULAR_HEADERS})
target_compile_features(tabular PUBLIC cxx_std_20)
target_include_directories(tabular PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${CMAKE_CURRENT_BINARY_DIR}/include
)

# The per-instance provenance ring (P16) is a Debug-only debugging aid. It is a generated
# header, not a compile definition, so it reaches only the units that include
# tabular/instance.h. Written at generate time, and only when its content changes.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/include/tabular/build_config.h"
	CONTENT "#define TABULAR_PROVENANCE $<IF:$<CONFIG:Debug>,1,0>\n")

if(TABULAR_TESTS)
	add_library(tabular_tests OBJECT ${TABULAR_TEST_SOURCES} ${TABULAR_TEST_HEADERS})
	# Usage requirements only: an object library takes no objects from another.
	target_link_libraries(tabular_tests PRIVATE tabular)
endif()
