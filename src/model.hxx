// model.hxx — the sqr ODBC driver's object model (layer 2).
//
// Environment / Connection / Statement carry all driver state and ODBC
// semantics — statement state machine, deferred parameter and column
// bindings, fetch conversions, transaction handling, diagnostics — but
// expose a plain C++ interface: no extern "C", no handle laundering.  The
// SQL* trampolines (layer 3) are thin wrappers over these classes.
//
// Ownership is a strict tree: an Environment owns its Connections, a
// Connection its Statements, each in a vector<unique_ptr<...>>.  The raw
// pointers handed out are non-owning views into that tree (they are what an
// ODBC handle is); freeing goes through the parent's free_* call.  There is
// no shared ownership anywhere and no raw new/delete.
//
// Every operation clears the handle's DiagStack, posts records on failure
// or warning, and returns an Rc that maps 1:1 onto SQLRETURN.

#pragma once

#include "diag.hxx"
#include "wire.hxx"

#include <sql.h>
#include <sqlext.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sqro {

// Operation outcome; the trampolines map this onto SQLRETURN.
enum class Rc { ok, ok_info, no_data, error };

// How one wire column type appears through ODBC eyes.  Shared by the
// catalogue builders (catalog.cxx, where it is defined) and by
// SQLDescribeCol / SQLColAttribute in the trampolines.
struct TypeFacts {
    SQLSMALLINT sql_type = 0;
    const char* type_name = "";
    int column_size = 0;
    bool is_char = false;
};
[[nodiscard]] TypeFacts type_facts(sqrw::ColType type, int csize);

class Connection;
class Statement;

class Environment {
public:
    // Out-of-line special members: the unique_ptr vectors need the owned
    // types complete, which they only are in model.cxx.
    Environment();
    ~Environment();
    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;

    DiagStack diag = {};

    // SQL_ATTR_ODBC_VERSION: the application's declared version (3 / 3.8).
    long odbc_version = SQL_OV_ODBC3;

    [[nodiscard]] Connection* create_connection();
    Rc free_connection(Connection* conn);      // HY010 while still connected
    [[nodiscard]] bool has_connections() const { return !connections_.empty(); }
    Rc end_tran(bool commit);                  // SQLEndTran on the environment: all connections

private:
    std::vector<std::unique_ptr<Connection>> connections_ = {};
};

class Connection {
public:
    explicit Connection(Environment& env);     // out-of-line: see Environment
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    DiagStack diag = {};
    [[nodiscard]] Environment& env() { return *env_; }

    // Connect-time identity, echoed back through SQLGetInfo
    // (SQL_DATA_SOURCE_NAME / SQL_SERVER_NAME) and SQLDriverConnect's out
    // string.  Owned by the trampolines.
    std::string dsn = {};
    std::string host = {};
    int port = 0;

    Rc connect(std::string_view host, int port);
    Rc disconnect();                            // 25000 with a transaction open
    [[nodiscard]] bool connected() const { return wire_.has_value(); }
    [[nodiscard]] const std::string& dbname() const;
    [[nodiscard]] const std::string& server_version() const;

    // SQL_ATTR_AUTOCOMMIT.  Off = the driver wraps work in BEGIN...COMMIT
    // itself (BEGIN lazily before the next statement, COMMIT/ROLLBACK via
    // end_tran).  Turning autocommit back on commits any open transaction.
    Rc set_autocommit(bool on);
    [[nodiscard]] bool autocommit() const { return autocommit_; }
    Rc end_tran(bool commit);                   // SQLEndTran
    [[nodiscard]] bool txn_open() const { return txn_open_; }

    [[nodiscard]] Statement* create_statement();  // nullptr + 08003 when not connected
    Rc free_statement(Statement* stmt);
    [[nodiscard]] bool has_statements() const { return !statements_.empty(); }

    // --- used by Statement ---
    [[nodiscard]] sqrw::Connection* wire() { return wire_ ? &*wire_ : nullptr; }
    // Lazily open the driver-managed transaction (autocommit off).
    [[nodiscard]] sqrw::Result<sqrw::None> ensure_txn();

private:
    Environment* env_;
    std::optional<sqrw::Connection> wire_ = {};
    std::vector<std::unique_ptr<Statement>> statements_ = {};
    bool autocommit_ = true;
    bool txn_open_ = false;
};

class Statement {
public:
    explicit Statement(Connection& conn) : conn_(&conn) {}

    DiagStack diag = {};
    [[nodiscard]] Connection& connection() { return *conn_; }

    // Fetch-status reporting (SQL_ATTR_ROWS_FETCHED_PTR / ROW_STATUS_PTR)
    // and the cursor name.  Owned and written by the trampolines; stored
    // here because they are per-statement state.
    SQLULEN* rows_fetched_ptr = nullptr;
    SQLUSMALLINT* row_status_ptr = nullptr;
    std::string cursor_name = {};

    // --- statement preparation and execution ---
    Rc prepare(std::string_view sql);
    Rc exec_direct(std::string_view sql);
    Rc execute();

    // Deferred input binding (SQLBindParameter): the buffers are READ AT
    // EXECUTE TIME, so the application may change their contents between
    // executes.  `number` is 1-based; `ind` may be nullptr (no NULLs, char
    // data NUL-terminated).
    Rc bind_param(int number, SQLSMALLINT ctype, SQLSMALLINT stype,
                  const void* value, const SQLLEN* ind);
    Rc reset_params();
    [[nodiscard]] int num_params() const;        // marker count of the prepared statement

    // --- result set ---
    [[nodiscard]] int num_result_cols() const;
    [[nodiscard]] const sqrw::Column* column(int i) const;   // 1-based; nullptr if out of range
    [[nodiscard]] SQLLEN row_count() const { return affected_; }
    [[nodiscard]] SQLLEN current_row() const { return static_cast<SQLLEN>(row_); }

    // Deferred output binding (SQLBindCol): buffers are WRITTEN AT FETCH
    // TIME.  ctype = 0 (SQL_UNBIND semantics) drops the binding.
    Rc bind_col(int i, SQLSMALLINT ctype, void* buf, SQLLEN buflen, SQLLEN* ind);
    Rc unbind_cols();

    Rc fetch();                                  // no_data past the last row
    Rc get_data(int i, SQLSMALLINT ctype, void* buf, SQLLEN buflen, SQLLEN* ind);
    [[nodiscard]] bool has_cursor() const { return cursor_open_; }
    Rc close_cursor();                           // 24000 without one
    Rc free_close();                             // SQL_CLOSE: like close_cursor but never an error

    // --- catalogue result sets (SQLTables / SQLColumns / SQLPrimaryKeys /
    //     SQLGetTypeInfo / SQLSpecialColumns / SQLStatistics /
    //     SQLForeignKeys); each opens a cursor over a synthesised set ---
    Rc catalog_tables(std::string_view name = {});         // exact-name filter; ""/"%" = all
    Rc catalog_columns(std::string_view table);
    Rc catalog_primary_keys(std::string_view table);
    Rc catalog_type_info(int data_type = SQL_ALL_TYPES);   // filtered unless SQL_ALL_TYPES
    Rc catalog_special_columns(std::string_view table,
                               bool best_rowid = true);    // best rowid = the unique key;
                                                           // false (ROWVER) = empty set
    Rc catalog_statistics(std::string_view table);         // the unique index, if any
    Rc catalog_foreign_keys();                             // always empty: sqr has none

private:
    struct ParamBinding {
        bool bound = false;
        SQLSMALLINT ctype = 0;
        SQLSMALLINT stype = 0;
        const void* value = nullptr;
        const SQLLEN* ind = nullptr;
    };
    struct ColBinding {
        SQLSMALLINT ctype = 0;       // 0 = unbound
        void* buf = nullptr;
        SQLLEN buflen = 0;
        SQLLEN* ind = nullptr;
    };

    enum class State { initial, prepared, executed, cursor };
    enum class Op { prepare, execute, fetch, get_data, close_cursor };

    [[nodiscard]] bool gate(Op op);              // state check; posts and returns false when illegal
    [[nodiscard]] State state() const;
    Rc run(std::string_view sql);                // send + classify the reply
    Rc open_synthetic(sqrw::RowSet rs);          // install a catalogue result set
    [[nodiscard]] const sqrw::RowSet* rowset() const { return rs_ ? &*rs_ : nullptr; }
    [[nodiscard]] sqrw::Result<std::string> format_param(int number) const;

    Connection* conn_;
    bool prepared_ = false;
    bool executed_ = false;
    std::vector<std::string> segments_ = {};     // SQL split at '?' markers (outside quotes)
    std::vector<ParamBinding> params_ = {};
    std::optional<sqrw::RowSet> rs_ = {};
    SQLLEN affected_ = 0;
    bool cursor_open_ = false;
    std::size_t row_ = 0;                        // rows fetched so far (current row = row_ - 1)
    std::vector<ColBinding> cols_ = {};
    int getdata_col_ = 0;                        // SQLGetData continuation state
    std::size_t getdata_off_ = 0;
    bool getdata_done_ = false;
};

} // namespace sqro
