// odbc_test.cxx — tests for the SQL* entry points (layer 3), driven through
// libsqrodbc.so and the plain C API: exactly the surface a driver manager
// loads.  A real sqrd is spawned on a scratch database; the connection goes
// through SQLDriverConnect with an explicit SERVER/PORT pair.
//
// Usage: odbc_test <path-to-sqrd>

#include "testutil.hxx"

#include <sql.h>
#include <sqlext.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <print>
#include <string>

namespace {

    using testutil::check;

    constexpr const char* dbdir = "odbc_test_db";

    [[nodiscard]] SQLCHAR* sqlstr(const char* s) {
        return reinterpret_cast<SQLCHAR*>(const_cast<char*>(s));
    }

    // The first diagnostic's SQLSTATE on a handle, "" if none.
    [[nodiscard]] std::string diag_state(SQLSMALLINT type, SQLHANDLE h) {
        std::array<SQLCHAR, 6> state = {};
        SQLINTEGER native = 0;
        std::array<SQLCHAR, 256> msg = {};
        SQLSMALLINT len = 0;
        if (SQLGetDiagRec(type, h, 1, state.data(), &native, msg.data(),
                          static_cast<SQLSMALLINT>(msg.size()), &len)
            != SQL_SUCCESS) {
            return {};
        }
        return std::string(reinterpret_cast<char*>(state.data()));
    }

    // Fetch to exhaustion.
    [[nodiscard]] int count_rows(SQLHSTMT stmt) {
        int n = 0;
        while (SQL_SUCCEEDED(SQLFetch(stmt))) {
            ++n;
        }
        return n;
    }

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::println(stderr, "usage: odbc_test <path-to-sqrd>");
        return 2;
    }
    std::filesystem::remove_all(dbdir);
    const auto srv = testutil::spawn_sqrd(argv[1], dbdir);
    check(srv.port > 0, "sqrd spawned on an ephemeral port");
    if (srv.port <= 0) {
        return 1;
    }

    // ---- environment and connection ----
    SQLHENV env = SQL_NULL_HANDLE;
    check(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env) == SQL_SUCCESS, "alloc env");
    check(SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION,
                        reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0) == SQL_SUCCESS,
          "declare ODBC 3");
    SQLHDBC dbc = SQL_NULL_HANDLE;
    check(SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc) == SQL_SUCCESS, "alloc dbc");
    check(SQLFreeHandle(SQL_HANDLE_ENV, env) == SQL_ERROR
              && diag_state(SQL_HANDLE_ENV, env) == "HY010",
          "cannot free env with a connection");

    const auto connstr =
        std::string("SERVER=127.0.0.1;PORT=") + std::to_string(srv.port) + ";UID=me;PWD={x}";
    std::array<SQLCHAR, 256> out = {};
    SQLSMALLINT outlen = 0;
    check(SQLDriverConnect(dbc, nullptr, sqlstr(connstr.c_str()), SQL_NTS, out.data(),
                           static_cast<SQLSMALLINT>(out.size()), &outlen,
                           SQL_DRIVER_NOPROMPT) == SQL_SUCCESS,
          "SQLDriverConnect");
    check(std::string(reinterpret_cast<char*>(out.data())).find("PORT=")
              != std::string::npos,
          "out connection string is canonical");
    check(SQLFetch(nullptr) == SQL_INVALID_HANDLE, "null handle is SQL_INVALID_HANDLE");

    // Braced values may contain ';' (the standard escape), and TIMEOUT is a
    // recognised key that lands in the connection attributes.
    {
        SQLHDBC dbc2 = SQL_NULL_HANDLE;
        check(SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc2) == SQL_SUCCESS, "alloc dbc2");
        const auto cs2 = std::string("PWD={a;PORT=99999};SERVER=127.0.0.1;PORT=")
                         + std::to_string(srv.port) + ";TIMEOUT=5";
        check(SQLDriverConnect(dbc2, nullptr, sqlstr(cs2.c_str()), SQL_NTS, nullptr, 0, nullptr,
                               SQL_DRIVER_NOPROMPT) == SQL_SUCCESS,
              "braced value containing ';' parses");
        SQLUINTEGER tmo = 0;
        check(SQLGetConnectAttr(dbc2, SQL_ATTR_CONNECTION_TIMEOUT, &tmo, 0, nullptr)
                      == SQL_SUCCESS
                  && tmo == 5,
              "TIMEOUT key reaches SQL_ATTR_CONNECTION_TIMEOUT");
        check(SQLDisconnect(dbc2) == SQL_SUCCESS
                  && SQLFreeHandle(SQL_HANDLE_DBC, dbc2) == SQL_SUCCESS,
              "dbc2 torn down");
    }

    // ---- SQLGetInfo / SQLGetFunctions ----
    {
        std::array<SQLCHAR, 64> buf = {};
        SQLSMALLINT n = 0;
        check(SQLGetInfo(dbc, SQL_DBMS_NAME, buf.data(),
                         static_cast<SQLSMALLINT>(buf.size()), &n) == SQL_SUCCESS
                  && std::string(reinterpret_cast<char*>(buf.data())) == "sqr",
              "SQL_DBMS_NAME");
        check(SQLGetInfo(dbc, SQL_DRIVER_ODBC_VER, buf.data(),
                         static_cast<SQLSMALLINT>(buf.size()), &n) == SQL_SUCCESS
                  && std::string(reinterpret_cast<char*>(buf.data())) == "03.80",
              "SQL_DRIVER_ODBC_VER");
        SQLUSMALLINT txn = 0;
        check(SQLGetInfo(dbc, SQL_TXN_CAPABLE, &txn, 0, nullptr) == SQL_SUCCESS
                  && txn == SQL_TC_ALL,
              "SQL_TXN_CAPABLE");
        SQLUINTEGER gd = 0;
        check(SQLGetInfo(dbc, SQL_GETDATA_EXTENSIONS, &gd, 0, nullptr) == SQL_SUCCESS
                  && (gd & SQL_GD_ANY_ORDER) != 0,
              "SQL_GETDATA_EXTENSIONS");
        check(SQLGetInfo(dbc, 9999, buf.data(), 0, nullptr) == SQL_ERROR
                  && diag_state(SQL_HANDLE_DBC, dbc) == "HY096",
              "unknown info type is HY096");

        SQLUSMALLINT sup = SQL_FALSE;
        check(SQLGetFunctions(dbc, SQL_API_SQLFETCH, &sup) == SQL_SUCCESS && sup == SQL_TRUE,
              "SQLGetFunctions: SQLFetch supported");
        check(SQLGetFunctions(dbc, SQL_API_SQLPUTDATA, &sup) == SQL_SUCCESS && sup == SQL_FALSE,
              "SQLGetFunctions: SQLPutData not supported");
        std::array<SQLUSMALLINT, SQL_API_ODBC3_ALL_FUNCTIONS_SIZE> bitmap = {};
        check(SQLGetFunctions(dbc, SQL_API_ODBC3_ALL_FUNCTIONS, bitmap.data()) == SQL_SUCCESS
                  && SQL_FUNC_EXISTS(bitmap.data(), SQL_API_SQLALLOCHANDLE) == SQL_TRUE
                  && SQL_FUNC_EXISTS(bitmap.data(), SQL_API_SQLPUTDATA) == SQL_FALSE,
              "SQLGetFunctions: ODBC3 bitmap");
    }

    // ---- DDL, prepared inserts with deferred buffers ----
    SQLHSTMT stmt = SQL_NULL_HANDLE;
    check(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) == SQL_SUCCESS, "alloc stmt");
    {
        SQLSMALLINT np = 0;
        check(SQLNumParams(stmt, &np) == SQL_ERROR
                  && diag_state(SQL_HANDLE_STMT, stmt) == "HY010",
              "SQLNumParams before prepare is HY010");
    }
    check(SQLExecDirect(stmt,
                        sqlstr("CREATE TABLE t (id INTEGER, x REAL, name CHAR(12), note TEXT)"),
                        SQL_NTS) == SQL_SUCCESS,
          "CREATE TABLE");
    check(SQLExecDirect(stmt, sqlstr("CREATE UNIQUE INDEX ON t (id)"), SQL_NTS) == SQL_SUCCESS,
          "CREATE UNIQUE INDEX");
    check(SQLExecDirect(stmt, sqlstr("SELEC oops"), SQL_NTS) == SQL_ERROR
              && diag_state(SQL_HANDLE_STMT, stmt) == "42000",
          "syntax error posts 42000 on the statement");
    {
        SQLINTEGER native = 0;
        std::array<SQLCHAR, 8> st = {};
        check(SQLGetDiagField(SQL_HANDLE_STMT, stmt, 0, SQL_DIAG_NUMBER, &native, 0, nullptr)
                      == SQL_SUCCESS
                  && native == 1,
              "SQL_DIAG_NUMBER");
        SQLSMALLINT n = 0;
        check(SQLGetDiagField(SQL_HANDLE_STMT, stmt, 1, SQL_DIAG_SQLSTATE, st.data(),
                              static_cast<SQLSMALLINT>(st.size()), &n) == SQL_SUCCESS
                  && std::string(reinterpret_cast<char*>(st.data())) == "42000",
              "SQL_DIAG_SQLSTATE field");
        SQLRETURN lastrc = 0;
        check(SQLGetDiagField(SQL_HANDLE_STMT, stmt, 0, SQL_DIAG_RETURNCODE, &lastrc, 0,
                              nullptr) == SQL_SUCCESS
                  && lastrc == SQL_ERROR,
              "SQL_DIAG_RETURNCODE");
    }

    check(SQLPrepare(stmt, sqlstr("INSERT INTO t VALUES (?, ?, ?, ?)"), SQL_NTS) == SQL_SUCCESS,
          "prepare with markers");
    SQLSMALLINT nparams = 0;
    check(SQLNumParams(stmt, &nparams) == SQL_SUCCESS && nparams == 4, "SQLNumParams");

    SQLINTEGER id = 1;
    double x = 0.5;
    std::array<char, 16> name = {};
    std::strcpy(name.data(), "alpha");
    std::array<char, 32> note = {};
    std::strcpy(note.data(), "first note");
    SQLLEN name_ind = SQL_NTS;
    SQLLEN note_ind = SQL_NTS;
    check(SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &id, 0,
                           nullptr) == SQL_SUCCESS
              && SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_DOUBLE, 0, 0, &x,
                                  0, nullptr) == SQL_SUCCESS
              && SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 12, 0,
                                  name.data(), 0, &name_ind) == SQL_SUCCESS
              && SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_LONGVARCHAR, 0, 0,
                                  note.data(), 0, &note_ind) == SQL_SUCCESS,
          "bind 4 parameters");
    SQLULEN nproc = 99;
    std::array<SQLUSMALLINT, 1> pstatus = {99};
    check(SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &nproc, 0) == SQL_SUCCESS
              && SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, pstatus.data(), 0)
                     == SQL_SUCCESS,
          "set param status pointers");
    check(SQLExecute(stmt) == SQL_SUCCESS && nproc == 1 && pstatus[0] == SQL_PARAM_SUCCESS,
          "execute insert 1 (param status reported)");
    SQLLEN affected = 0;
    check(SQLRowCount(stmt, &affected) == SQL_SUCCESS && affected == 1, "SQLRowCount");

    // Deferred buffers: change contents, execute again.
    id = 2;
    x = 1.5;
    std::strcpy(name.data(), "beta");
    note_ind = SQL_NULL_DATA;
    check(SQLExecute(stmt) == SQL_SUCCESS, "execute insert 2 (buffers re-read)");
    check(SQLBindParameter(stmt, 1, SQL_PARAM_OUTPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &id, 0,
                           nullptr) == SQL_ERROR
              && diag_state(SQL_HANDLE_STMT, stmt) == "HYC00",
          "output parameters refused");
    {
        // the indicator is deferred: data-at-exec is accepted at bind time
        // and refused at execute time
        SQLLEN daexec = SQL_DATA_AT_EXEC;
        check(SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_LONGVARCHAR, 0, 0,
                               note.data(), 0, &daexec) == SQL_SUCCESS,
              "data-at-exec indicator accepted at bind time");
        check(SQLExecute(stmt) == SQL_ERROR && diag_state(SQL_HANDLE_STMT, stmt) == "HYC00",
              "data-at-exec refused at execute time");
        check(nproc == 0 && pstatus[0] == SQL_PARAM_ERROR,
              "failed execute reports param error status");
    }

    // ---- SELECT: describe, bind, fetch, get data ----
    check(SQLExecDirect(stmt, sqlstr("SELECT id, x, name, note FROM t ORDER BY id"), SQL_NTS)
              == SQL_SUCCESS,
          "SELECT");
    SQLSMALLINT ncols = 0;
    check(SQLNumResultCols(stmt, &ncols) == SQL_SUCCESS && ncols == 4, "SQLNumResultCols");
    {
        std::array<SQLCHAR, 64> cname = {};
        SQLSMALLINT namelen = 0;
        SQLSMALLINT dtype = 0;
        SQLULEN csize = 0;
        SQLSMALLINT digits = 0;
        SQLSMALLINT nullable = 0;
        check(SQLDescribeCol(stmt, 1, cname.data(), static_cast<SQLSMALLINT>(cname.size()),
                             &namelen, &dtype, &csize, &digits, &nullable) == SQL_SUCCESS
                  && std::string(reinterpret_cast<char*>(cname.data())) == "id"
                  && dtype == SQL_INTEGER && nullable == SQL_NULLABLE,
              "SQLDescribeCol column 1");
        check(SQLDescribeCol(stmt, 3, cname.data(), 3, &namelen, &dtype, &csize, &digits,
                             &nullable) == SQL_SUCCESS_WITH_INFO
                  && namelen == 4,
              "SQLDescribeCol truncates the name with 01004");
        SQLLEN nval = 0;
        check(SQLColAttribute(stmt, 0, SQL_DESC_COUNT, nullptr, 0, nullptr, &nval)
                      == SQL_SUCCESS
                  && nval == 4,
              "SQLColAttribute SQL_DESC_COUNT");
        check(SQLColAttribute(stmt, 2, SQL_DESC_CONCISE_TYPE, nullptr, 0, nullptr, &nval)
                      == SQL_SUCCESS
                  && nval == SQL_DOUBLE,
              "SQLColAttribute SQL_DESC_CONCISE_TYPE");
        check(SQLColAttribute(stmt, 1, SQL_DESC_DISPLAY_SIZE, nullptr, 0, nullptr, &nval)
                      == SQL_SUCCESS
                  && nval == 11,
              "SQLColAttribute SQL_DESC_DISPLAY_SIZE");
        check(SQLColAttribute(stmt, 4, SQL_DESC_NAME, cname.data(),
                              static_cast<SQLSMALLINT>(cname.size()), &namelen, nullptr)
                      == SQL_SUCCESS
                  && std::string(reinterpret_cast<char*>(cname.data())) == "note",
              "SQLColAttribute SQL_DESC_NAME");
    }
    {
        SQLINTEGER got_id = 0;
        double got_x = 0.0;
        SQLLEN id_ind = 0;
        SQLLEN x_ind = 0;
        SQLULEN nfetched = 0;
        check(SQLSetStmtAttr(stmt, SQL_ATTR_ROWS_FETCHED_PTR, &nfetched, 0) == SQL_SUCCESS,
              "set SQL_ATTR_ROWS_FETCHED_PTR");
        check(SQLBindCol(stmt, 1, SQL_C_SLONG, &got_id, 0, &id_ind) == SQL_SUCCESS
                  && SQLBindCol(stmt, 2, SQL_C_DOUBLE, &got_x, 0, &x_ind) == SQL_SUCCESS,
              "bind result columns");
        check(SQLFetch(stmt) == SQL_SUCCESS && got_id == 1 && got_x == 0.5 && nfetched == 1,
              "fetch row 1 into bound columns");

        // Chunked SQLGetData on the CHAR column: "alpha" through a 3-byte
        // buffer.
        std::array<char, 3> chunk = {};
        SQLLEN ind = 0;
        std::string assembled;
        check(SQLGetData(stmt, 3, SQL_C_CHAR, chunk.data(), chunk.size(), &ind)
                      == SQL_SUCCESS_WITH_INFO
                  && ind == 5,
              "GetData chunk 1 truncates with full length");
        assembled += chunk.data();
        while (true) {
            const auto rc = SQLGetData(stmt, 3, SQL_C_CHAR, chunk.data(), chunk.size(), &ind);
            if (rc == SQL_NO_DATA) {
                break;
            }
            assembled += chunk.data();
            if (rc == SQL_SUCCESS) {
                continue;
            }
        }
        check(assembled == "alpha", "GetData chunks reassemble");

        check(SQLFetch(stmt) == SQL_SUCCESS && got_id == 2 && got_x == 1.5, "fetch row 2");
        check(SQLGetData(stmt, 4, SQL_C_CHAR, chunk.data(), chunk.size(), &ind) == SQL_SUCCESS
                  && ind == SQL_NULL_DATA,
              "GetData NULL note");
        check(SQLGetData(stmt, 1, SQL_C_SLONG, nullptr, 0, &ind) == SQL_ERROR
                  && diag_state(SQL_HANDLE_STMT, stmt) == "HY009",
              "GetData with a null buffer is HY009");
        SQLBIGINT big = 0;
        check(SQLGetData(stmt, 1, SQL_C_SBIGINT, &big, sizeof big, &ind) == SQL_SUCCESS
                  && big == 2 && ind == 8,
              "GetData SQL_C_SBIGINT");
        check(SQLFetch(stmt) == SQL_NO_DATA && nfetched == 0, "fetch past the end");
        check(SQLMoreResults(stmt) == SQL_NO_DATA, "SQLMoreResults");
        check(SQLCloseCursor(stmt) == SQL_SUCCESS, "SQLCloseCursor");
        check(SQLCloseCursor(stmt) == SQL_ERROR
                  && diag_state(SQL_HANDLE_STMT, stmt) == "24000",
              "closing a closed cursor is 24000");
        check(SQLFreeStmt(stmt, SQL_UNBIND) == SQL_SUCCESS
                  && SQLFreeStmt(stmt, SQL_RESET_PARAMS) == SQL_SUCCESS,
              "SQLFreeStmt unbind/reset");
    }

    // ---- SQLFetchScroll (forward only) ----
    check(SQLExecDirect(stmt, sqlstr("SELECT id FROM t ORDER BY id"), SQL_NTS) == SQL_SUCCESS,
          "SELECT for FetchScroll");
    {
        SQLLEN diag_rows = 0;
        check(SQLGetDiagField(SQL_HANDLE_STMT, stmt, 0, SQL_DIAG_CURSOR_ROW_COUNT, &diag_rows,
                              0, nullptr) == SQL_SUCCESS
                  && diag_rows == 2,
              "SQL_DIAG_CURSOR_ROW_COUNT");
    }
    check(SQLFetchScroll(stmt, SQL_FETCH_NEXT, 0) == SQL_SUCCESS, "FetchScroll NEXT");
    check(SQLFetchScroll(stmt, SQL_FETCH_PRIOR, 0) == SQL_ERROR
              && diag_state(SQL_HANDLE_STMT, stmt) == "HY106",
          "FetchScroll PRIOR is HY106");
    check(SQLFreeStmt(stmt, SQL_CLOSE) == SQL_SUCCESS, "SQLFreeStmt SQL_CLOSE");

    // ---- cursor names ----
    {
        std::array<SQLCHAR, 32> cur = {};
        SQLSMALLINT n = 0;
        check(SQLGetCursorName(stmt, cur.data(), static_cast<SQLSMALLINT>(cur.size()), &n)
                      == SQL_SUCCESS
                  && n > 0,
              "generated cursor name");
        check(SQLSetCursorName(stmt, sqlstr("mycursor"), SQL_NTS) == SQL_SUCCESS, "set cursor name");
        check(SQLGetCursorName(stmt, cur.data(), static_cast<SQLSMALLINT>(cur.size()), &n)
                      == SQL_SUCCESS
                  && std::string(reinterpret_cast<char*>(cur.data())) == "mycursor",
              "cursor name round-trips");
    }

    // ---- transactions through the C API ----
    check(SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT,
                            reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0) == SQL_SUCCESS,
          "autocommit off");
    check(SQLExecDirect(stmt, sqlstr("INSERT INTO t VALUES (3, 2.5, 'gamma', NULL)"), SQL_NTS)
              == SQL_SUCCESS,
          "insert inside driver-managed transaction");
    check(SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK) == SQL_SUCCESS, "SQLEndTran rollback");
    check(SQLExecDirect(stmt, sqlstr("SELECT id FROM t"), SQL_NTS) == SQL_SUCCESS
              && count_rows(stmt) == 2,
          "rollback discarded the insert");
    check(SQLFreeStmt(stmt, SQL_CLOSE) == SQL_SUCCESS, "close after count");
    check(SQLEndTran(SQL_HANDLE_ENV, env, SQL_COMMIT) == SQL_SUCCESS,
          "environment-level SQLEndTran");
    check(SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT,
                            reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0) == SQL_SUCCESS,
          "autocommit back on");
    {
        SQLUINTEGER ac = 0;
        check(SQLGetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT, &ac, 0, nullptr) == SQL_SUCCESS
                  && ac == SQL_AUTOCOMMIT_ON,
              "SQLGetConnectAttr autocommit");
    }

    // ---- catalogue entry points ----
    check(SQLTables(stmt, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0) == SQL_SUCCESS
              && count_rows(stmt) == 1,
          "SQLTables lists t");
    check(SQLFreeStmt(stmt, SQL_CLOSE) == SQL_SUCCESS
              && SQLTables(stmt, nullptr, 0, nullptr, 0, nullptr, 0, sqlstr("VIEW"), SQL_NTS)
                     == SQL_SUCCESS
              && count_rows(stmt) == 0,
          "SQLTables type filter VIEW is empty");
    check(SQLFreeStmt(stmt, SQL_CLOSE) == SQL_SUCCESS
              && SQLTables(stmt, nullptr, 0, nullptr, 0, nullptr, 0, sqlstr("SYSTEM TABLE"),
                           SQL_NTS) == SQL_SUCCESS
              && count_rows(stmt) == 0,
          "SQLTables SYSTEM TABLE does not match TABLE");
    check(SQLFreeStmt(stmt, SQL_CLOSE) == SQL_SUCCESS
              && SQLTables(stmt, nullptr, 0, nullptr, 0, nullptr, 0, sqlstr("VIEW,'TABLE'"),
                           SQL_NTS) == SQL_SUCCESS
              && count_rows(stmt) == 1,
          "SQLTables quoted TABLE in a type list matches");
    check(SQLFreeStmt(stmt, SQL_CLOSE) == SQL_SUCCESS
              && SQLColumns(stmt, nullptr, 0, nullptr, 0, sqlstr("t"), SQL_NTS, nullptr, 0)
                     == SQL_SUCCESS
              && count_rows(stmt) == 4,
          "SQLColumns has 4 rows");
    check(SQLFreeStmt(stmt, SQL_CLOSE) == SQL_SUCCESS
              && SQLColumns(stmt, nullptr, 0, nullptr, 0, sqlstr("t"), SQL_NTS, sqlstr("name"),
                            SQL_NTS) == SQL_SUCCESS
              && count_rows(stmt) == 1,
          "SQLColumns column filter keeps one row");
    check(SQLFreeStmt(stmt, SQL_CLOSE) == SQL_SUCCESS
              && SQLPrimaryKeys(stmt, nullptr, 0, nullptr, 0, sqlstr("t"), SQL_NTS)
                     == SQL_SUCCESS
              && count_rows(stmt) == 1,
          "SQLPrimaryKeys has the key column");
    check(SQLFreeStmt(stmt, SQL_CLOSE) == SQL_SUCCESS
              && SQLGetTypeInfo(stmt, SQL_ALL_TYPES) == SQL_SUCCESS && count_rows(stmt) == 4,
          "SQLGetTypeInfo all types");
    check(SQLFreeStmt(stmt, SQL_CLOSE) == SQL_SUCCESS
              && SQLGetTypeInfo(stmt, SQL_INTEGER) == SQL_SUCCESS && count_rows(stmt) == 1,
          "SQLGetTypeInfo filtered to INTEGER");
    check(SQLFreeStmt(stmt, SQL_CLOSE) == SQL_SUCCESS
              && SQLSpecialColumns(stmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0, sqlstr("t"),
                                   SQL_NTS, SQL_SCOPE_SESSION, SQL_NULLABLE) == SQL_SUCCESS
              && count_rows(stmt) == 1,
          "SQLSpecialColumns best rowid = key");
    check(SQLFreeStmt(stmt, SQL_CLOSE) == SQL_SUCCESS
              && SQLSpecialColumns(stmt, SQL_ROWVER, nullptr, 0, nullptr, 0, sqlstr("t"),
                                   SQL_NTS, SQL_SCOPE_SESSION, SQL_NULLABLE) == SQL_SUCCESS
              && count_rows(stmt) == 0,
          "SQLSpecialColumns ROWVER is empty");
    check(SQLFreeStmt(stmt, SQL_CLOSE) == SQL_SUCCESS
              && SQLStatistics(stmt, nullptr, 0, nullptr, 0, sqlstr("t"), SQL_NTS,
                               SQL_INDEX_UNIQUE, SQL_QUICK) == SQL_SUCCESS
              && count_rows(stmt) == 1,
          "SQLStatistics reports the unique index");
    check(SQLFreeStmt(stmt, SQL_CLOSE) == SQL_SUCCESS
              && SQLForeignKeys(stmt, nullptr, 0, nullptr, 0, sqlstr("t"), SQL_NTS, nullptr, 0,
                                nullptr, 0, nullptr, 0) == SQL_SUCCESS
              && count_rows(stmt) == 0,
          "SQLForeignKeys is empty");
    check(SQLFreeStmt(stmt, SQL_CLOSE) == SQL_SUCCESS, "close catalogue cursor");

    // regression: SQLExecute on a statement whose only history is a
    // catalogue set must be HY010 after the cursor closes, not a crash
    {
        SQLHSTMT stmt2 = SQL_NULL_HANDLE;
        check(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt2) == SQL_SUCCESS
                  && SQLTables(stmt2, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0)
                         == SQL_SUCCESS
                  && SQLFreeStmt(stmt2, SQL_CLOSE) == SQL_SUCCESS
                  && SQLExecute(stmt2) == SQL_ERROR
                  && diag_state(SQL_HANDLE_STMT, stmt2) == "HY010",
              "SQLExecute after a closed catalogue cursor is HY010");
        check(SQLFreeHandle(SQL_HANDLE_STMT, stmt2) == SQL_SUCCESS, "free regression stmt");
    }

    // ---- SQLNativeSql ----
    {
        std::array<SQLCHAR, 64> native = {};
        SQLINTEGER n = 0;
        check(SQLNativeSql(dbc, sqlstr("SELECT 1"), SQL_NTS, native.data(),
                           static_cast<SQLINTEGER>(native.size()), &n) == SQL_SUCCESS
                  && std::string(reinterpret_cast<char*>(native.data())) == "SELECT 1",
              "SQLNativeSql passes through");
    }

    // ---- teardown ordering ----
    check(SQLFreeHandle(SQL_HANDLE_STMT, stmt) == SQL_SUCCESS, "free stmt");
    check(SQLFreeHandle(SQL_HANDLE_DBC, dbc) == SQL_ERROR
              && diag_state(SQL_HANDLE_DBC, dbc) == "HY010",
          "cannot free a connected dbc");
    check(SQLDisconnect(dbc) == SQL_SUCCESS, "disconnect");
    check(SQLFreeHandle(SQL_HANDLE_DBC, dbc) == SQL_SUCCESS, "free dbc");
    check(SQLFreeHandle(SQL_HANDLE_ENV, env) == SQL_SUCCESS, "free env");

    testutil::stop_sqrd(srv);
    std::filesystem::remove_all(dbdir);
    return testutil::summary("odbc_test");
}
