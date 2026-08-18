// catalog.cxx — synthesised catalogue result sets (layer 2).
//
// SQLTables / SQLColumns / SQLPrimaryKeys / SQLGetTypeInfo all deliver their
// answers as ordinary fetchable result sets on the statement handle, so the
// builders here shape the wire layer's TABLES/COLUMNS replies (or fixed
// data, for type info) into the column layouts the ODBC specification
// prescribes, and install them with open_synthetic().  sqr has no
// catalogs or schemas, so those columns are always NULL.

#include "model.hxx"

#include <algorithm>
#include <utility>

namespace sqro {

namespace {

    using sqrw::Cell;
    using sqrw::ColType;
    using sqrw::Column;
    using sqrw::RowSet;

    constexpr int name_size = 32;      // SQR_NAME_LEN: table/column name cap

    // Shorthands for building synthetic sets.
    [[nodiscard]] Column col(std::string name, ColType type, int csize) {
        return {.name = std::move(name), .type = type, .csize = csize,
                .nullable = true, .key_ordinal = 0};
    }
    [[nodiscard]] Cell S(std::string s) { return Cell{std::move(s)}; }
    [[nodiscard]] Cell I(int v) { return Cell{static_cast<std::int32_t>(v)}; }
    [[nodiscard]] Cell N() { return Cell{std::monostate{}}; }

} // namespace

TypeFacts type_facts(ColType type, int csize) {
    switch (type) {
    case ColType::Int:
        return {.sql_type = SQL_INTEGER, .type_name = "INTEGER",
                .column_size = 10, .is_char = false};
    case ColType::Real:
        return {.sql_type = SQL_DOUBLE, .type_name = "DOUBLE",
                .column_size = 15, .is_char = false};
    case ColType::Char:
        return {.sql_type = SQL_VARCHAR, .type_name = "VARCHAR",
                .column_size = csize, .is_char = true};
    case ColType::Text:
        return {.sql_type = SQL_LONGVARCHAR, .type_name = "LONGVARCHAR",
                .column_size = static_cast<int>(sqrw::max_payload), .is_char = true};
    }
    return {};
}

Rc Statement::catalog_tables(std::string_view name) {
    diag.clear();
    auto* wire = conn_->wire();
    if (wire == nullptr) {
        diag.post("08003", "connection not open");
        return Rc::error;
    }
    auto names = wire->tables();
    if (!names) {
        diag.post(names.error());
        return Rc::error;
    }
    RowSet rs;
    rs.columns = {col("TABLE_CAT", ColType::Char, name_size),
                  col("TABLE_SCHEM", ColType::Char, name_size),
                  col("TABLE_NAME", ColType::Char, name_size),
                  col("TABLE_TYPE", ColType::Char, name_size),
                  col("REMARKS", ColType::Char, name_size)};
    // Exact-name filtering only: sqr has no pattern matching, so anything
    // other than ""/"%" is taken literally.
    rs.rows.reserve(names->size());
    for (auto& tname : *names) {
        if (name.empty() || name == "%" || name == tname) {
            rs.rows.push_back({N(), N(), S(std::move(tname)), S("TABLE"), N()});
        }
    }
    return open_synthetic(std::move(rs));
}

Rc Statement::catalog_columns(std::string_view table, std::string_view column) {
    diag.clear();
    auto* wire = conn_->wire();
    if (wire == nullptr) {
        diag.post("08003", "connection not open");
        return Rc::error;
    }
    auto cols = wire->columns(table);
    if (!cols) {
        diag.post(cols.error());
        return Rc::error;
    }
    RowSet rs;
    rs.columns = {col("TABLE_CAT", ColType::Char, name_size),
                  col("TABLE_SCHEM", ColType::Char, name_size),
                  col("TABLE_NAME", ColType::Char, name_size),
                  col("COLUMN_NAME", ColType::Char, name_size),
                  col("DATA_TYPE", ColType::Int, 4),
                  col("TYPE_NAME", ColType::Char, name_size),
                  col("COLUMN_SIZE", ColType::Int, 4),
                  col("BUFFER_LENGTH", ColType::Int, 4),
                  col("DECIMAL_DIGITS", ColType::Int, 4),
                  col("NUM_PREC_RADIX", ColType::Int, 4),
                  col("NULLABLE", ColType::Int, 4),
                  col("REMARKS", ColType::Char, name_size),
                  col("COLUMN_DEF", ColType::Char, name_size),
                  col("SQL_DATA_TYPE", ColType::Int, 4),
                  col("SQL_DATETIME_SUB", ColType::Int, 4),
                  col("CHAR_OCTET_LENGTH", ColType::Int, 4),
                  col("ORDINAL_POSITION", ColType::Int, 4),
                  col("IS_NULLABLE", ColType::Char, 4)};
    rs.rows.reserve(cols->size());
    int ordinal = 0;
    for (auto& c : *cols) {
        // ORDINAL_POSITION is the position in the table, so it advances
        // whether or not the column survives the filter (exact-name only,
        // the same convention as tables).
        ++ordinal;
        if (!column.empty() && column != "%" && column != c.name) {
            continue;
        }
        const auto f = type_facts(c.type, c.csize);
        rs.rows.push_back({N(),                        // TABLE_CAT
                           N(),                        // TABLE_SCHEM
                           S(std::string(table)),      // TABLE_NAME
                           S(std::move(c.name)),       // COLUMN_NAME
                           I(f.sql_type),              // DATA_TYPE
                           S(f.type_name),             // TYPE_NAME
                           I(f.column_size),           // COLUMN_SIZE
                           I(c.type == ColType::Int ? 4
                             : c.type == ColType::Real ? 8
                                                       : f.column_size),  // BUFFER_LENGTH
                           c.type == ColType::Int ? I(0) : N(),   // DECIMAL_DIGITS
                           f.is_char ? N() : I(10),    // NUM_PREC_RADIX
                           I(SQL_NULLABLE),            // NULLABLE (sqr has no NOT NULL)
                           N(),                        // REMARKS
                           N(),                        // COLUMN_DEF
                           I(f.sql_type),              // SQL_DATA_TYPE
                           N(),                        // SQL_DATETIME_SUB
                           f.is_char ? I(f.column_size) : N(),    // CHAR_OCTET_LENGTH
                           I(ordinal),                 // ORDINAL_POSITION
                           S("YES")});                 // IS_NULLABLE
    }
    return open_synthetic(std::move(rs));
}

Rc Statement::catalog_primary_keys(std::string_view table) {
    diag.clear();
    auto* wire = conn_->wire();
    if (wire == nullptr) {
        diag.post("08003", "connection not open");
        return Rc::error;
    }
    auto cols = wire->columns(table);
    if (!cols) {
        diag.post(cols.error());
        return Rc::error;
    }
    RowSet rs;
    rs.columns = {col("TABLE_CAT", ColType::Char, name_size),
                  col("TABLE_SCHEM", ColType::Char, name_size),
                  col("TABLE_NAME", ColType::Char, name_size),
                  col("COLUMN_NAME", ColType::Char, name_size),
                  col("KEY_SEQ", ColType::Int, 4),
                  col("PK_NAME", ColType::Char, name_size)};
    // Key members in ordinal order (they arrive in declaration order).
    for (int seq = 1;; ++seq) {
        const auto at = std::ranges::find_if(*cols,
                                             [seq](const auto& c) { return c.key_ordinal == seq; });
        if (at == cols->end()) {
            break;
        }
        rs.rows.push_back({N(), N(), S(std::string(table)), S(at->name), I(seq), N()});
    }
    return open_synthetic(std::move(rs));
}

Rc Statement::catalog_type_info(int data_type) {
    diag.clear();
    RowSet rs;
    rs.columns = {col("TYPE_NAME", ColType::Char, name_size),
                  col("DATA_TYPE", ColType::Int, 4),
                  col("COLUMN_SIZE", ColType::Int, 4),
                  col("LITERAL_PREFIX", ColType::Char, 4),
                  col("LITERAL_SUFFIX", ColType::Char, 4),
                  col("CREATE_PARAMS", ColType::Char, 16),
                  col("NULLABLE", ColType::Int, 4),
                  col("CASE_SENSITIVE", ColType::Int, 4),
                  col("SEARCHABLE", ColType::Int, 4),
                  col("UNSIGNED_ATTRIBUTE", ColType::Int, 4),
                  col("FIXED_PREC_SCALE", ColType::Int, 4),
                  col("AUTO_UNIQUE_VALUE", ColType::Int, 4),
                  col("LOCAL_TYPE_NAME", ColType::Char, name_size),
                  col("MINIMUM_SCALE", ColType::Int, 4),
                  col("MAXIMUM_SCALE", ColType::Int, 4),
                  col("SQL_DATA_TYPE", ColType::Int, 4),
                  col("SQL_DATETIME_SUB", ColType::Int, 4),
                  col("NUM_PREC_RADIX", ColType::Int, 4),
                  col("INTERVAL_PRECISION", ColType::Int, 4)};
    // One row per wire type.  SEARCHABLE is SQL_PRED_BASIC throughout: every
    // type supports comparison predicates but none supports LIKE (sqr has
    // no LIKE).  Numerics are signed, exact/approximate per type.
    const auto row = [](const char* name, SQLSMALLINT sql_type, int size,
                        bool is_char, bool has_scale) {
        return std::vector<Cell>{
            S(name),
            I(sql_type),
            I(size),
            is_char ? S("'") : N(),               // LITERAL_PREFIX
            is_char ? S("'") : N(),               // LITERAL_SUFFIX
            sql_type == SQL_VARCHAR ? S("length") : N(),  // CREATE_PARAMS
            I(SQL_NULLABLE),
            I(is_char ? SQL_TRUE : SQL_FALSE),    // CASE_SENSITIVE
            I(SQL_PRED_BASIC),                    // SEARCHABLE
            is_char ? N() : I(SQL_FALSE),         // UNSIGNED_ATTRIBUTE (signed)
            I(SQL_FALSE),                         // FIXED_PREC_SCALE
            is_char ? N() : I(SQL_FALSE),         // AUTO_UNIQUE_VALUE
            N(),                                  // LOCAL_TYPE_NAME
            has_scale ? I(0) : N(),               // MINIMUM_SCALE
            has_scale ? I(0) : N(),               // MAXIMUM_SCALE
            I(sql_type),                          // SQL_DATA_TYPE
            N(),                                  // SQL_DATETIME_SUB
            is_char ? N() : I(10),                // NUM_PREC_RADIX
            N(),                                  // INTERVAL_PRECISION
        };
    };
    rs.rows.push_back(row("INTEGER", SQL_INTEGER, 10, false, true));
    rs.rows.push_back(row("DOUBLE", SQL_DOUBLE, 15, false, false));
    // The engine bounds CHAR(n) only by the record cap (SQR_MAX_RECORD,
    // 1 MiB); 65535 is a deliberately conservative advertisement.
    rs.rows.push_back(row("VARCHAR", SQL_VARCHAR, 65535, true, false));
    rs.rows.push_back(row("LONGVARCHAR", SQL_LONGVARCHAR,
                          static_cast<int>(sqrw::max_payload), true, false));
    if (data_type != SQL_ALL_TYPES) {
        std::erase_if(rs.rows, [data_type](const auto& r) {
            return std::get<std::int32_t>(r[1]) != data_type;   // r[1] = DATA_TYPE
        });
    }
    return open_synthetic(std::move(rs));
}

Rc Statement::catalog_special_columns(std::string_view table, bool best_rowid) {
    diag.clear();
    auto* wire = conn_->wire();
    if (wire == nullptr) {
        diag.post("08003", "connection not open");
        return Rc::error;
    }
    auto cols = wire->columns(table);
    if (!cols) {
        diag.post(cols.error());
        return Rc::error;
    }
    RowSet rs;
    rs.columns = {col("SCOPE", ColType::Int, 4),
                  col("COLUMN_NAME", ColType::Char, name_size),
                  col("DATA_TYPE", ColType::Int, 4),
                  col("TYPE_NAME", ColType::Char, name_size),
                  col("COLUMN_SIZE", ColType::Int, 4),
                  col("BUFFER_LENGTH", ColType::Int, 4),
                  col("DECIMAL_DIGITS", ColType::Int, 4),
                  col("PSEUDO_COLUMN", ColType::Int, 4)};
    // The best rowid is the designated unique key, in ordinal order; the key
    // survives any command, hence SQL_SCOPE_SESSION.  A ROWVER request gets
    // the shape with no rows: sqr has no version columns.
    for (int seq = 1; best_rowid; ++seq) {
        const auto at = std::ranges::find_if(*cols,
                                             [seq](const auto& c) { return c.key_ordinal == seq; });
        if (at == cols->end()) {
            break;
        }
        const auto f = type_facts(at->type, at->csize);
        rs.rows.push_back({I(SQL_SCOPE_SESSION),
                           S(at->name),
                           I(f.sql_type),
                           S(f.type_name),
                           I(f.column_size),
                           I(at->type == ColType::Int ? 4
                             : at->type == ColType::Real ? 8
                                                         : f.column_size),
                           at->type == ColType::Int ? I(0) : N(),
                           I(SQL_PC_NOT_PSEUDO)});
    }
    return open_synthetic(std::move(rs));
}

Rc Statement::catalog_statistics(std::string_view table) {
    diag.clear();
    auto* wire = conn_->wire();
    if (wire == nullptr) {
        diag.post("08003", "connection not open");
        return Rc::error;
    }
    auto cols = wire->columns(table);
    if (!cols) {
        diag.post(cols.error());
        return Rc::error;
    }
    RowSet rs;
    rs.columns = {col("TABLE_CAT", ColType::Char, name_size),
                  col("TABLE_SCHEM", ColType::Char, name_size),
                  col("TABLE_NAME", ColType::Char, name_size),
                  col("NON_UNIQUE", ColType::Int, 4),
                  col("INDEX_QUALIFIER", ColType::Char, name_size),
                  col("INDEX_NAME", ColType::Char, name_size),
                  col("TYPE", ColType::Int, 4),
                  col("ORDINAL_POSITION", ColType::Int, 4),
                  col("COLUMN_NAME", ColType::Char, name_size),
                  col("ASC_OR_DESC", ColType::Char, 4),
                  col("CARDINALITY", ColType::Int, 4),
                  col("PAGES", ColType::Int, 4),
                  col("FILTER_CONDITION", ColType::Char, name_size)};
    // One row per member of the designated unique index (sqr's indexes have
    // no names on the wire; "KEY" stands in).
    for (int seq = 1;; ++seq) {
        const auto at = std::ranges::find_if(*cols,
                                             [seq](const auto& c) { return c.key_ordinal == seq; });
        if (at == cols->end()) {
            break;
        }
        rs.rows.push_back({N(), N(), S(std::string(table)),
                           I(SQL_FALSE),                 // NON_UNIQUE
                           N(),
                           S("KEY"),
                           I(SQL_INDEX_OTHER),
                           I(seq),
                           S(at->name),
                           S("A"),
                           N(), N(), N()});
    }
    return open_synthetic(std::move(rs));
}

Rc Statement::catalog_foreign_keys() {
    diag.clear();
    // sqr has no foreign keys: the correct answer is the right shape, empty.
    RowSet rs;
    rs.columns = {col("PKTABLE_CAT", ColType::Char, name_size),
                  col("PKTABLE_SCHEM", ColType::Char, name_size),
                  col("PKTABLE_NAME", ColType::Char, name_size),
                  col("PKCOLUMN_NAME", ColType::Char, name_size),
                  col("FKTABLE_CAT", ColType::Char, name_size),
                  col("FKTABLE_SCHEM", ColType::Char, name_size),
                  col("FKTABLE_NAME", ColType::Char, name_size),
                  col("FKCOLUMN_NAME", ColType::Char, name_size),
                  col("KEY_SEQ", ColType::Int, 4),
                  col("UPDATE_RULE", ColType::Int, 4),
                  col("DELETE_RULE", ColType::Int, 4),
                  col("FK_NAME", ColType::Char, name_size),
                  col("PK_NAME", ColType::Char, name_size),
                  col("DEFERRABILITY", ColType::Int, 4)};
    return open_synthetic(std::move(rs));
}

} // namespace sqro
