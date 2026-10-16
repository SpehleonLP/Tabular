#ifndef TABULAR_SCHEMA_YAML_H
#define TABULAR_SCHEMA_YAML_H

#include "tabular/relation.h"
#include <string_view>

namespace tabular
{

// A YAML document of table definitions, built into a schema. label names the
// source in errors. The document is
//
//   tables:
//     - name: spawn_points
//       columns:
//         - {name: package, type: hash, key: true}
//         - {name: x, type: double}
//       order_by: {columns: [package], unique: true}   # optional
//       read_only: true                                # optional
//
// and type is one of hash, flags, int, double, condition, command.
tl::expected<schema, ErrorContext> SchemaFromYaml(std::string_view text, std::string_view label);

}

#endif // TABULAR_SCHEMA_YAML_H
