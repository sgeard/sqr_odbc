// model_test.cxx — tests for the object model (layer 2) against a real
// spawned sqrd: statement lifecycle and state gates, deferred parameter and
// column bindings, fetch/GetData conversions, transactions and the
// synthesised catalogue result sets.  The SQL* trampolines (layer 3) are a
// thin skin over exactly these calls.
//
// Usage: model_test <path-to-sqrd>

#include "model.hxx"
#include "testutil.hxx"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <print>
#include <string>

namespace {

    using testutil::check;
    using sqro::Rc;

    constexpr const char* dbdir = "model_test_db";

    // The first diagnostic's SQLSTATE on a handle-like object, "" if none.
    template <class H>
    std::string state_of(const H& h) {
        const auto* rec = h.diag.record(1);
        return rec != nullptr ? rec->sqlstate : std::string{};
    }

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::println(stderr, "usage: model_test <path-to-sqrd>");
        return 2;
    }
    std::filesystem::remove_all(dbdir);
    const auto srv = testutil::spawn_sqrd(argv[1], dbdir);
    check(srv.port > 0, "sqrd spawned on an ephemeral port");
    if (srv.port <= 0) {
        return 1;
    }

    {
        // ---- environment / connection lifecycle ----
        sqro::Environment env;
        auto* dbc = env.create_connection();
        check(dbc != nullptr, "connection allocated");
        check(env.free_connection(nullptr) == Rc::error, "freeing an unknown handle is refused");
        check(dbc->connect("127.0.0.1", 1) == Rc::error && state_of(*dbc) == "08001",
              "failed connect posts 08001");
        check(dbc->connect("127.0.0.1", srv.port) == Rc::ok, "connect");
        check(dbc->connect("127.0.0.1", srv.port) == Rc::error && state_of(*dbc) == "08002",
              "double connect posts 08002");
        check(dbc->dbname() == dbdir, "dbname visible");
        check(env.free_connection(dbc) == Rc::error && state_of(env) == "HY010",
              "cannot free a connected connection");

        auto* stmt = dbc->create_statement();
        check(stmt != nullptr, "statement allocated");

        // ---- exec_direct, state gates ----
        check(stmt->execute() == Rc::error && state_of(*stmt) == "HY010",
              "execute before prepare is HY010");
        check(stmt->fetch() == Rc::error && state_of(*stmt) == "24000",
              "fetch without a cursor is 24000");
        check(stmt->exec_direct("CREATE TABLE t (id INTEGER, x REAL, name CHAR(12), note TEXT)")
                  == Rc::ok,
              "CREATE TABLE");
        check(stmt->exec_direct("CREATE UNIQUE INDEX ON t (id)") == Rc::ok, "CREATE UNIQUE INDEX");
        check(stmt->exec_direct("SELEC oops") == Rc::error && state_of(*stmt) == "42000",
              "syntax error maps to 42000");
        check(stmt->exec_direct("SELECT * FROM nosuch") == Rc::error
                  && state_of(*stmt) == "42S02",
              "missing table maps to 42S02");

        // ---- prepared statement with deferred parameter buffers ----
        check(stmt->prepare("INSERT INTO t VALUES (?, ?, ?, ?)") == Rc::ok, "prepare with markers");
        SQLINTEGER id = 1;
        double x = 0.1;
        char name[16] = "Alice";
        char note[32] = "first";
        SQLLEN name_ind = SQL_NTS;
        SQLLEN note_ind = SQL_NTS;
        check(stmt->execute() == Rc::error && state_of(*stmt) == "07002",
              "execute with unbound parameters is 07002");
        check(stmt->bind_param(1, SQL_C_SLONG, SQL_INTEGER, &id, nullptr) == Rc::ok
                  && stmt->bind_param(2, SQL_C_DOUBLE, SQL_DOUBLE, &x, nullptr) == Rc::ok
                  && stmt->bind_param(3, SQL_C_CHAR, SQL_VARCHAR, name, &name_ind) == Rc::ok
                  && stmt->bind_param(4, SQL_C_CHAR, SQL_LONGVARCHAR, note, &note_ind) == Rc::ok,
              "parameters bound");
        check(stmt->execute() == Rc::ok && stmt->row_count() == 1, "execute inserts row 1");
        // deferred buffers: change the contents, execute again
        id = 2;
        x = -1.5e-300;
        std::strcpy(name, "O'Brien");
        note_ind = SQL_NULL_DATA;
        check(stmt->execute() == Rc::ok, "re-execute reads the changed buffers");
        id = 3;
        x = 1.0 / 3.0;
        std::strcpy(name, "42");        // a number into a CHAR column, quoted by SQL type
        note_ind = SQL_NTS;
        std::strcpy(note, "a longer note");   // 13 chars: will truncate into out_note[8]
        check(stmt->execute() == Rc::ok, "third row");

        // ---- fetch with bound columns and conversions ----
        check(stmt->exec_direct("SELECT * FROM t ORDER BY id") == Rc::ok, "SELECT");
        check(stmt->num_result_cols() == 4 && stmt->column(2)->type == sqrw::ColType::Real,
              "result metadata");
        SQLINTEGER out_id = 0;
        double out_x = 0;
        char out_name[16] = {};
        char out_note[8] = {};          // deliberately small: truncation
        SQLLEN ind_id = 0;
        SQLLEN ind_x = 0;
        SQLLEN ind_name = 0;
        SQLLEN ind_note = 0;
        check(stmt->bind_col(1, SQL_C_SLONG, &out_id, sizeof out_id, &ind_id) == Rc::ok
                  && stmt->bind_col(2, SQL_C_DOUBLE, &out_x, sizeof out_x, &ind_x) == Rc::ok
                  && stmt->bind_col(3, SQL_C_CHAR, out_name, sizeof out_name, &ind_name) == Rc::ok
                  && stmt->bind_col(4, SQL_C_CHAR, out_note, sizeof out_note, &ind_note) == Rc::ok,
              "columns bound");
        check(stmt->fetch() == Rc::ok && out_id == 1 && out_x == 0.1
                  && std::strcmp(out_name, "Alice") == 0 && std::strcmp(out_note, "first") == 0
                  && ind_name == 5,
              "row 1 fetched, conversions exact");
        check(stmt->fetch() == Rc::ok && out_id == 2 && out_x == -1.5e-300
                  && std::strcmp(out_name, "O'Brien") == 0 && ind_note == SQL_NULL_DATA,
              "row 2: extreme REAL and NULL indicator");
        const auto rc3 = stmt->fetch();
        check(rc3 == Rc::ok_info && state_of(*stmt) == "01004"
                  && std::strcmp(out_note, "a longe") == 0 && ind_note == 13
                  && out_x == 1.0 / 3.0,
              "row 3: to_chars literal exact; long note truncates with 01004");
        check(stmt->fetch() == Rc::no_data, "fetch past the end is no_data");

        // ---- GetData: conversions and chunked character retrieval ----
        check(stmt->close_cursor() == Rc::ok, "close cursor");
        check(stmt->close_cursor() == Rc::error && state_of(*stmt) == "24000",
              "closing again is 24000");
        check(stmt->exec_direct("SELECT id, x, note FROM t WHERE id = 1") == Rc::ok
                  && stmt->fetch() == Rc::ok,
              "single row for GetData");
        char digits[16] = {};
        SQLLEN dig_ind = 0;
        check(stmt->get_data(1, SQL_C_CHAR, digits, sizeof digits, &dig_ind) == Rc::ok
                  && std::strcmp(digits, "1") == 0 && dig_ind == 1,
              "int -> char conversion");
        double as_real = 0;
        check(stmt->get_data(1, SQL_C_DOUBLE, &as_real, sizeof as_real, nullptr) == Rc::no_data,
              "repeating a delivered column is no_data (whatever the C type)");
        check(stmt->get_data(2, SQL_C_DOUBLE, &as_real, sizeof as_real, nullptr) == Rc::ok
                  && as_real == 0.1,
              "real -> double via GetData is exact");
        // chunked: "first" through a 3-byte buffer = 2 chars + NUL per call
        char chunk[3] = {};
        SQLLEN chunk_ind = 0;
        std::string assembled;
        auto rc = stmt->get_data(3, SQL_C_CHAR, chunk, sizeof chunk, &chunk_ind);
        check(rc == Rc::ok_info && chunk_ind == 5, "first chunk truncates, full length reported");
        assembled += chunk;
        while (rc == Rc::ok_info) {
            rc = stmt->get_data(3, SQL_C_CHAR, chunk, sizeof chunk, &chunk_ind);
            if (rc == Rc::ok || rc == Rc::ok_info) {
                assembled += chunk;
            }
        }
        check(rc == Rc::ok && assembled == "first", "chunks reassemble the value");
        check(stmt->get_data(3, SQL_C_CHAR, chunk, sizeof chunk, &chunk_ind) == Rc::no_data,
              "then no_data");
        check(stmt->get_data(9, SQL_C_CHAR, chunk, sizeof chunk, &chunk_ind) == Rc::error
                  && state_of(*stmt) == "07009",
              "bad column index is 07009");

        // ---- numeric edge conversions ----
        // (bindings persist across statements, so unbind before reusing stmt)
        check(stmt->unbind_cols() == Rc::ok && stmt->free_close() == Rc::ok,
              "unbind + SQL_CLOSE");
        check(stmt->exec_direct("SELECT id, x FROM t WHERE id = 1") == Rc::ok
                  && stmt->fetch() == Rc::ok,
              "numeric edge row");
        double idr = 0;
        check(stmt->get_data(1, SQL_C_DOUBLE, &idr, sizeof idr, nullptr) == Rc::ok && idr == 1.0,
              "int -> double conversion");
        SQLINTEGER whole = 0;
        check(stmt->get_data(2, SQL_C_SLONG, &whole, sizeof whole, nullptr) == Rc::ok_info
                  && whole == 0 && state_of(*stmt) == "01S07",
              "double -> int truncates the fraction with 01S07");

        // ---- transactions: autocommit off, rollback, BUSY on a second connection ----
        check(dbc->set_autocommit(false) == Rc::ok, "autocommit off");
        check(stmt->free_close() == Rc::ok, "cursor closed before DML");
        check(stmt->exec_direct("INSERT INTO t (id) VALUES (50)") == Rc::ok,
              "write opens the driver-managed transaction");
        check(dbc->txn_open(), "transaction is open");

        auto* dbc2 = env.create_connection();
        check(dbc2->connect("127.0.0.1", srv.port) == Rc::ok, "second connection");
        auto* stmt2 = dbc2->create_statement();
        check(stmt2->exec_direct("DELETE FROM t WHERE id = 1") == Rc::error
                  && state_of(*stmt2) == "40001",
              "cross-connection write maps BUSY to 40001");
        check(stmt2->exec_direct("SELECT id FROM t WHERE id = 1") == Rc::ok,
              "cross-connection read allowed");
        check(stmt2->free_close() == Rc::ok, "second cursor closed");

        check(dbc->disconnect() == Rc::error && state_of(*dbc) == "25000",
              "disconnect with an open transaction is 25000");
        check(dbc->end_tran(false) == Rc::ok && !dbc->txn_open(), "rollback");
        check(stmt->exec_direct("SELECT id FROM t WHERE id = 50") == Rc::ok
                  && stmt->fetch() == Rc::no_data,
              "rolled-back row is gone");
        check(stmt->free_close() == Rc::ok, "cursor closed");
        check(stmt->exec_direct("INSERT INTO t (id) VALUES (60)") == Rc::ok
                  && dbc->set_autocommit(true) == Rc::ok && !dbc->txn_open(),
              "autocommit back on commits the open transaction");
        check(stmt2->exec_direct("DELETE FROM t WHERE id = 60") == Rc::ok
                  && stmt2->row_count() == 1,
              "committed row visible to the other connection");

        // ---- catalogue result sets ----
        check(stmt->catalog_tables() == Rc::ok && stmt->num_result_cols() == 5
                  && stmt->fetch() == Rc::ok,
              "SQLTables shape");
        char tname[33] = {};
        check(stmt->get_data(3, SQL_C_CHAR, tname, sizeof tname, nullptr) == Rc::ok
                  && std::strcmp(tname, "t") == 0,
              "TABLE_NAME cell");
        check(stmt->free_close() == Rc::ok
                  && stmt->catalog_columns("t") == Rc::ok && stmt->num_result_cols() == 18,
              "SQLColumns is 18 columns");
        {
            SQLINTEGER data_type = 0;
            SQLLEN dt_ind = 0;
            check(stmt->bind_col(5, SQL_C_SLONG, &data_type, sizeof data_type, &dt_ind) == Rc::ok
                      && stmt->fetch() == Rc::ok && data_type == SQL_INTEGER
                      && stmt->fetch() == Rc::ok && data_type == SQL_DOUBLE
                      && stmt->fetch() == Rc::ok && data_type == SQL_VARCHAR
                      && stmt->fetch() == Rc::ok && data_type == SQL_LONGVARCHAR,
                  "DATA_TYPE per column");
            check(stmt->unbind_cols() == Rc::ok && stmt->free_close() == Rc::ok, "reset");
        }
        check(stmt->catalog_columns("nosuch") == Rc::error && state_of(*stmt) == "42S02",
              "SQLColumns on a missing table is 42S02");
        check(stmt->catalog_primary_keys("t") == Rc::ok && stmt->fetch() == Rc::ok,
              "SQLPrimaryKeys has the key row");
        {
            char pk[33] = {};
            SQLINTEGER seq = 0;
            check(stmt->get_data(4, SQL_C_CHAR, pk, sizeof pk, nullptr) == Rc::ok
                      && std::strcmp(pk, "id") == 0
                      && stmt->get_data(5, SQL_C_SLONG, &seq, sizeof seq, nullptr) == Rc::ok
                      && seq == 1,
                  "key column and sequence");
            check(stmt->fetch() == Rc::no_data && stmt->free_close() == Rc::ok,
                  "single-column key");
        }
        check(stmt->catalog_type_info() == Rc::ok && stmt->num_result_cols() == 19
                  && stmt->row_count() == 4,
              "SQLGetTypeInfo lists the four types");
        check(stmt->free_close() == Rc::ok, "type-info cursor closed");

        // ---- teardown ----
        check(dbc->free_statement(stmt) == Rc::ok, "statement freed");
        check(dbc2->disconnect() == Rc::ok && env.free_connection(dbc2) == Rc::ok,
              "second connection torn down");
        check(dbc->disconnect() == Rc::ok, "disconnect");
        check(env.free_connection(dbc) == Rc::ok, "connection freed");
        check(!env.has_connections(), "environment empty");
    }

    testutil::stop_sqrd(srv);
    std::filesystem::remove_all(dbdir);
    return testutil::summary("model tests");
}
