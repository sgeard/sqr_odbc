// odbc.cxx — the extern "C" SQL* entry points (layer 3).
//
// A thin skin over the sqro object model: each entry point casts its handle,
// runs the corresponding model call inside guard() (which turns escaping
// exceptions into posted diagnostics and maps Rc onto SQLRETURN), and copies
// results into the application's buffers.  ANSI only — a driver that exports
// no W variants is an ANSI driver, and unixODBC converts on our behalf.
//
// Everything here is local except the SQL* symbols themselves: the shared
// library is built -fvisibility=hidden with a version script exporting SQL*
// only.

#include "model.hxx"

#include <odbcinst.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sqro::DiagStack;
using sqro::Rc;

// The driver's identity as reported through SQLGetInfo.
inline constexpr const char* driver_name = "libsqrodbc.so";
inline constexpr const char* driver_version = "01.00.0000";   // ODBC ##.##.#### form
inline constexpr const char* driver_odbc_version = "03.80";
// sqrd's conventional port (the same default as the Calc client); a DSN or
// connection string with an explicit PORT overrides it.
inline constexpr int default_port = 7477;

// ---- handles ------------------------------------------------------------
// An ODBC handle is a non-owning pointer into the model's ownership tree;
// environments, the roots of that tree, are owned here.  The registry mutex
// covers only environment alloc/free — everything below an environment is
// single-threaded by design, like the wire protocol itself.

std::mutex& env_mutex() {
    static std::mutex m;
    return m;
}

std::vector<std::unique_ptr<sqro::Environment>>& environments() {
    static std::vector<std::unique_ptr<sqro::Environment>> envs;
    return envs;
}

[[nodiscard]] sqro::Environment* as_env(SQLHANDLE h) { return static_cast<sqro::Environment*>(h); }
[[nodiscard]] sqro::Connection* as_dbc(SQLHANDLE h) { return static_cast<sqro::Connection*>(h); }
[[nodiscard]] sqro::Statement* as_stmt(SQLHANDLE h) { return static_cast<sqro::Statement*>(h); }

// ---- outcome mapping ----------------------------------------------------

[[nodiscard]] SQLRETURN to_sqlreturn(Rc rc) {
    switch (rc) {
    case Rc::ok:      return SQL_SUCCESS;
    case Rc::ok_info: return SQL_SUCCESS_WITH_INFO;
    case Rc::no_data: return SQL_NO_DATA;
    case Rc::error:   return SQL_ERROR;
    case Rc::invalid: return SQL_INVALID_HANDLE;
    }
    return SQL_ERROR;
}

// Run one entry point's body on a handle: null check, exception fence,
// Rc -> SQLRETURN.  The body (or the model call inside it) is responsible
// for clearing the handle's diagnostics.
template <class Handle, class Body>
[[nodiscard]] SQLRETURN guard(Handle* h, Body&& body) {
    if (h == nullptr) {
        return SQL_INVALID_HANDLE;
    }
    SQLRETURN ret = SQL_ERROR;
    try {
        ret = to_sqlreturn(body());
    } catch (const std::bad_alloc&) {
        h->diag.post("HY001", "memory allocation failure");
    } catch (const std::exception& e) {
        h->diag.post("HY000", e.what());
    } catch (...) {
        h->diag.post("HY000", "unexpected exception");
    }
    h->diag.set_return_code(ret);          // SQL_DIAG_RETURNCODE
    return ret;
}

// ---- argument and buffer helpers ----------------------------------------

// A driver-manager string argument: SQLCHAR* plus a length that may be
// SQL_NTS.
[[nodiscard]] std::string_view arg_str(const SQLCHAR* s, long len) {
    if (s == nullptr) {
        return {};
    }
    const auto* p = reinterpret_cast<const char*>(s);
    // Any negative length counts as SQL_NTS: a garbage negative must not
    // become a huge string_view.
    return len < 0 ? std::string_view(p)
                   : std::string_view(p, static_cast<std::size_t>(len));
}

// Copy a string into an application buffer, always NUL-terminating and
// reporting the untruncated length; 01004 + ok_info on truncation.
template <class LenT>
Rc copy_str(std::string_view sv, void* buf, LenT buflen, LenT* outlen, DiagStack& diag) {
    if (outlen != nullptr) {
        *outlen = static_cast<LenT>(sv.size());
    }
    if (buf == nullptr) {
        return Rc::ok;                 // length-only enquiry
    }
    if (buflen <= 0) {
        diag.post("HY090", "invalid string or buffer length");
        return Rc::error;
    }
    auto* out = static_cast<char*>(buf);
    const auto ncopy = std::min(sv.size(), static_cast<std::size_t>(buflen) - 1);
    std::memcpy(out, sv.data(), ncopy);
    out[ncopy] = '\0';
    if (ncopy < sv.size()) {
        diag.post("01004", "string data, right truncated");
        return Rc::ok_info;
    }
    return Rc::ok;
}

// Write a fixed-size value into an application buffer (memcpy: the buffer's
// alignment is the application's promise, not ours to assume).
template <class T>
void put_num(SQLPOINTER out, T v) {
    if (out != nullptr) {
        std::memcpy(out, &v, sizeof v);
    }
}

// An integer attribute value arrives as the pointer itself.
[[nodiscard]] SQLULEN attr_int(SQLPOINTER v) {
    return static_cast<SQLULEN>(reinterpret_cast<std::uintptr_t>(v));
}

// ---- connection strings and DSNs ----------------------------------------

struct ConnParams {
    std::string dsn = {};
    std::string server = {};
    std::string port = {};
    std::string timeout = {};
    // Not a selector — sqrd serves exactly one database, chosen when it was
    // started, and the protocol has no way to ask for a different one.  It
    // is an ASSERTION: what the DSN believes it is pointing at.  A DSN whose
    // only identifying detail is a port number can silently reach the wrong
    // database when instances are restarted or ports reused, which is
    // precisely how a Base document ended up on a CAD scratch database.
    std::string database = {};
};

// Parse "KEY=value;KEY={value};..." (case-insensitive keys).  A braced value
// is taken to the matching '}', so it may contain ';' (the standard escape).
[[nodiscard]] ConnParams parse_conn_str(std::string_view s) {
    ConnParams p;
    std::size_t i = 0;
    while (i < s.size()) {
        const auto semi = s.find(';', i);
        const auto eq = s.find('=', i);
        if (eq == std::string_view::npos || (semi != std::string_view::npos && semi < eq)) {
            // No '=' in this item: skip it.
            if (semi == std::string_view::npos) {
                break;
            }
            i = semi + 1;
            continue;
        }
        std::string key(s.substr(i, eq - i));
        std::ranges::transform(key, key.begin(),
                               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        std::string_view val;
        if (eq + 1 < s.size() && s[eq + 1] == '{') {
            const auto close = s.find('}', eq + 2);
            if (close == std::string_view::npos) {
                val = s.substr(eq + 2);
                i = s.size();
            } else {
                val = s.substr(eq + 2, close - (eq + 2));
                const auto next = s.find(';', close);
                i = next == std::string_view::npos ? s.size() : next + 1;
            }
        } else {
            const auto end = s.find(';', eq + 1);
            val = s.substr(eq + 1, (end == std::string_view::npos ? s.size() : end) - (eq + 1));
            i = end == std::string_view::npos ? s.size() : end + 1;
        }
        if (key == "DSN") {
            p.dsn = val;
        } else if (key == "SERVER" || key == "HOST") {
            p.server = val;
        } else if (key == "PORT") {
            p.port = val;
        } else if (key == "TIMEOUT") {
            p.timeout = val;
        } else if (key == "DATABASE" || key == "DB") {
            p.database = val;
        }
        // Anything else (DRIVER, UID, PWD, ...) is accepted and ignored:
        // sqrd has no authentication.
    }
    return p;
}

// The port sqrd advertises in `<db-dir>/_sqrd`, or 0 if there is no usable
// one.  A DSN can then name the database directory and leave PORT out
// entirely, which is the address a user actually knows.  The file is only a
// hint — nothing removes it when a daemon dies — so the port it yields is
// worth nothing until the DATABASE assertion below confirms, over the
// connection, which database is really being served.
[[nodiscard]] int advertised_port(const std::string& dir) {
    std::error_code ec;
    const auto path = std::filesystem::path(dir) / "_sqrd";
    if (!std::filesystem::exists(path, ec) || ec) {
        return 0;
    }
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find(" = ");
        if (eq == std::string::npos || line.substr(0, eq) != "port") {
            continue;
        }
        const auto val = std::string_view(line).substr(eq + 3);
        int port = 0;
        const auto [end, pec] = std::from_chars(val.data(), val.data() + val.size(), port);
        if (pec == std::errc{} && end == val.data() + val.size() && port > 0 && port <= 65535) {
            return port;
        }
    }
    return 0;
}

// Does `want` name the database this connection reached?  Deliberately
// permissive: the short name, the directory as the server was told to open
// it, its fully resolved form, and either of those normalised — a symbolic
// link is a legitimate way to name a database, not a mistake to correct, so
// any spelling that lands on the same directory is accepted.
[[nodiscard]] bool database_matches(const sqro::Connection& dbc, const std::string& want) {
    if (want.empty() || want == dbc.dbname()) {
        return true;
    }
    const auto norm = [](const std::string& p) {
        if (p.empty()) {
            return std::string{};
        }
        std::error_code ec;
        const auto c = std::filesystem::weakly_canonical(p, ec);
        return ec ? std::string{} : c.string();
    };
    const std::array wants = {want, norm(want)};
    const std::array haves = {dbc.dbdir(), dbc.dbrealdir(), norm(dbc.dbdir()), norm(dbc.dbrealdir())};
    for (const auto& w : wants) {
        if (w.empty()) {
            continue;
        }
        if (std::ranges::contains(haves, w)) {
            return true;
        }
    }
    return false;
}

// odbc.ini lookup for a DSN entry.
[[nodiscard]] std::string profile(const std::string& dsn, const char* key, const char* fallback) {
    std::array<char, 256> buf = {};
    const int n = SQLGetPrivateProfileString(dsn.c_str(), key, fallback, buf.data(),
                                             static_cast<int>(buf.size()), "odbc.ini");
    return std::string(buf.data(), n > 0 ? static_cast<std::size_t>(n) : 0);
}

// Resolve parameters (explicit keys beat the DSN entry beats the default)
// and connect.
Rc do_connect(sqro::Connection* dbc, ConnParams p) {
    if (p.server.empty()) {
        p.server = p.dsn.empty() ? "127.0.0.1" : profile(p.dsn, "SERVER", "127.0.0.1");
    }
    if (p.timeout.empty() && !p.dsn.empty()) {
        p.timeout = profile(p.dsn, "TIMEOUT", "");
    }
    if (p.database.empty() && !p.dsn.empty()) {
        p.database = profile(p.dsn, "DATABASE", "");
    }
    // DATABASE is resolved before PORT so that naming the directory is
    // enough: an explicit PORT still wins, and the well-known default is
    // still the last resort.
    if (p.port.empty() && !p.database.empty()) {
        if (const int advertised = advertised_port(p.database); advertised > 0) {
            p.port = std::to_string(advertised);
        }
    }
    if (p.port.empty()) {
        p.port = p.dsn.empty() ? std::to_string(default_port)
                               : profile(p.dsn, "PORT", std::to_string(default_port).c_str());
    }
    int port = 0;
    const auto [end, ec] = std::from_chars(p.port.data(), p.port.data() + p.port.size(), port);
    if (ec != std::errc{} || end != p.port.data() + p.port.size() || port <= 0) {
        dbc->diag.clear();
        dbc->diag.post("08001", std::format("invalid PORT value '{}'", p.port));
        return Rc::error;
    }
    if (!p.timeout.empty()) {
        int seconds = 0;
        const auto [tend, tec] =
            std::from_chars(p.timeout.data(), p.timeout.data() + p.timeout.size(), seconds);
        if (tec != std::errc{} || tend != p.timeout.data() + p.timeout.size() || seconds < 0) {
            dbc->diag.clear();
            dbc->diag.post("08001", std::format("invalid TIMEOUT value '{}'", p.timeout));
            return Rc::error;
        }
        dbc->set_timeout(seconds);
    }
    const auto rc = dbc->connect(p.server, port);
    if (rc != Rc::ok) {
        return rc;
    }
    // Verify the assertion before handing the application a usable
    // connection: reaching the wrong database silently is worse than
    // failing to connect.  Either the short name or the absolute directory
    // satisfies it, so a DSN can say whichever is more stable for its site.
    if (!database_matches(*dbc, p.database)) {
        const auto actual = dbc->dbdir().empty() ? dbc->dbname()
                                                 : std::format("{} ({})", dbc->dbname(), dbc->dbdir());
        (void)dbc->disconnect();
        dbc->diag.clear();
        dbc->diag.post("08001", std::format("DATABASE is '{}' but {}:{} serves {}", p.database,
                                            p.server, port, actual));
        return Rc::error;
    }
    dbc->dsn = std::move(p.dsn);
    dbc->host = std::move(p.server);
    dbc->port = port;
    dbc->database = std::move(p.database);
    return rc;
}

// The canonical form of what we connected to, for the out connection string.
[[nodiscard]] std::string canonical_conn_str(const sqro::Connection& dbc) {
    std::string s;
    if (!dbc.dsn.empty()) {
        s = std::format("DSN={};", dbc.dsn);
    }
    s += std::format("SERVER={};PORT={}", dbc.host, dbc.port);
    if (!dbc.database.empty()) {
        s += std::format(";DATABASE={}", dbc.database);
    }
    return s;
}

// ---- SQLGetInfo ----------------------------------------------------------

enum class InfoKind { str, u16, u32 };

struct InfoEntry {
    SQLUSMALLINT type = 0;
    InfoKind kind = InfoKind::str;
    const char* s = "";
    SQLUINTEGER n = 0;
};

[[nodiscard]] constexpr InfoEntry info_s(SQLUSMALLINT type, const char* s) {
    return {.type = type, .kind = InfoKind::str, .s = s, .n = 0};
}
[[nodiscard]] constexpr InfoEntry info_16(SQLUSMALLINT type, SQLUINTEGER n) {
    return {.type = type, .kind = InfoKind::u16, .s = "", .n = n};
}
[[nodiscard]] constexpr InfoEntry info_32(SQLUSMALLINT type, SQLUINTEGER n) {
    return {.type = type, .kind = InfoKind::u32, .s = "", .n = n};
}

// Every static answer; the handful of dynamic ones (server version, database
// name, ...) are handled in SQLGetInfo itself.  Identifiers are
// case-sensitive, quoted or not, hence SQL_IC_SENSITIVE for both; sqr's SQL
// accepts standard double-quoted (delimited) identifiers, so applications
// (LibreOffice Base in particular) may use names with spaces and
// punctuation by quoting them.
constexpr std::array info_table = {
    info_s(SQL_DRIVER_NAME, driver_name),
    info_s(SQL_DRIVER_VER, driver_version),
    info_s(SQL_DRIVER_ODBC_VER, driver_odbc_version),
    info_s(SQL_DBMS_NAME, "sqr"),
    info_s(SQL_USER_NAME, ""),
    info_s(SQL_CATALOG_NAME, "N"),
    info_s(SQL_CATALOG_TERM, ""),
    info_s(SQL_CATALOG_NAME_SEPARATOR, ""),
    info_s(SQL_SCHEMA_TERM, ""),
    info_s(SQL_TABLE_TERM, "table"),
    info_s(SQL_PROCEDURES, "N"),
    info_s(SQL_PROCEDURE_TERM, ""),
    info_s(SQL_ACCESSIBLE_TABLES, "Y"),
    info_s(SQL_ACCESSIBLE_PROCEDURES, "N"),
    info_s(SQL_DATA_SOURCE_READ_ONLY, "N"),
    info_s(SQL_EXPRESSIONS_IN_ORDERBY, "N"),
    info_s(SQL_ORDER_BY_COLUMNS_IN_SELECT, "Y"),
    info_s(SQL_IDENTIFIER_QUOTE_CHAR, "\""),
    info_s(SQL_SEARCH_PATTERN_ESCAPE, ""),
    info_s(SQL_KEYWORDS, ""),
    info_s(SQL_SPECIAL_CHARACTERS, ""),
    info_s(SQL_OUTER_JOINS, "N"),
    info_s(SQL_COLUMN_ALIAS, "N"),
    info_s(SQL_LIKE_ESCAPE_CLAUSE, "N"),
    info_s(SQL_MULT_RESULT_SETS, "N"),
    info_s(SQL_MULTIPLE_ACTIVE_TXN, "Y"),
    info_s(SQL_NEED_LONG_DATA_LEN, "N"),
    info_s(SQL_DESCRIBE_PARAMETER, "N"),
    info_s(SQL_ROW_UPDATES, "N"),
    info_16(SQL_TXN_CAPABLE, SQL_TC_ALL),
    info_16(SQL_IDENTIFIER_CASE, SQL_IC_SENSITIVE),
    info_16(SQL_QUOTED_IDENTIFIER_CASE, SQL_IC_SENSITIVE),
    // Result sets are fully materialised client-side, so ending a
    // transaction disturbs no open cursor.
    info_16(SQL_CURSOR_COMMIT_BEHAVIOR, SQL_CB_PRESERVE),
    info_16(SQL_CURSOR_ROLLBACK_BEHAVIOR, SQL_CB_PRESERVE),
    info_16(SQL_NON_NULLABLE_COLUMNS, SQL_NNC_NULL),
    info_16(SQL_CONCAT_NULL_BEHAVIOR, SQL_CB_NULL),
    info_16(SQL_CORRELATION_NAME, SQL_CN_NONE),
    info_16(SQL_FILE_USAGE, SQL_FILE_NOT_SUPPORTED),
    info_16(SQL_GROUP_BY, SQL_GB_NOT_SUPPORTED),
    info_16(SQL_MAX_DRIVER_CONNECTIONS, 0),
    info_16(SQL_MAX_CONCURRENT_ACTIVITIES, 0),
    info_16(SQL_MAX_IDENTIFIER_LEN, 32),          // SQR_NAME_LEN
    info_16(SQL_MAX_COLUMN_NAME_LEN, 32),
    info_16(SQL_MAX_TABLE_NAME_LEN, 32),
    info_16(SQL_ACTIVE_ENVIRONMENTS, 0),
    info_16(SQL_NULL_COLLATION, SQL_NC_LOW),
    // Materialised rows: SQLGetData works on any column, in any order,
    // bound or not.
    info_32(SQL_GETDATA_EXTENSIONS, SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER | SQL_GD_BOUND),
    // One writer at a time with BUSY for everyone else is serializable.
    info_32(SQL_DEFAULT_TXN_ISOLATION, SQL_TXN_SERIALIZABLE),
    info_32(SQL_TXN_ISOLATION_OPTION, SQL_TXN_SERIALIZABLE),
    info_32(SQL_SCROLL_OPTIONS, SQL_SO_FORWARD_ONLY),
    info_32(SQL_ODBC_INTERFACE_CONFORMANCE, SQL_OIC_CORE),
    info_32(SQL_SQL_CONFORMANCE, SQL_SC_SQL92_ENTRY),
    info_32(SQL_CATALOG_USAGE, 0),
    info_32(SQL_SCHEMA_USAGE, 0),
    info_32(SQL_MAX_STATEMENT_LEN, sqrw::max_payload),
    info_32(SQL_STRING_FUNCTIONS, 0),
    info_32(SQL_NUMERIC_FUNCTIONS, 0),
    info_32(SQL_TIMEDATE_FUNCTIONS, 0),
    info_32(SQL_SYSTEM_FUNCTIONS, 0),
    info_32(SQL_CONVERT_FUNCTIONS, 0),
    info_32(SQL_ALTER_TABLE, 0),
    info_32(SQL_OJ_CAPABILITIES, 0),
    info_32(SQL_AGGREGATE_FUNCTIONS, 0),
    info_32(SQL_INSERT_STATEMENT, SQL_IS_INSERT_LITERALS),
    info_32(SQL_UNION, 0),
    info_32(SQL_SUBQUERIES, 0),
    info_32(SQL_DATETIME_LITERALS, 0),
    info_32(SQL_CREATE_TABLE, SQL_CT_CREATE_TABLE),
    info_32(SQL_DROP_TABLE, SQL_DT_DROP_TABLE),
    info_32(SQL_DDL_INDEX, SQL_DI_CREATE_INDEX | SQL_DI_DROP_INDEX),
    info_32(SQL_CURSOR_SENSITIVITY, SQL_INSENSITIVE),
    info_32(SQL_BOOKMARK_PERSISTENCE, 0),
    info_32(SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1, SQL_CA1_NEXT),
    info_32(SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2, SQL_CA2_READ_ONLY_CONCURRENCY),
    info_32(SQL_STATIC_CURSOR_ATTRIBUTES1, 0),
    info_32(SQL_STATIC_CURSOR_ATTRIBUTES2, 0),
    info_32(SQL_DYNAMIC_CURSOR_ATTRIBUTES1, 0),
    info_32(SQL_DYNAMIC_CURSOR_ATTRIBUTES2, 0),
    info_32(SQL_KEYSET_CURSOR_ATTRIBUTES1, 0),
    info_32(SQL_KEYSET_CURSOR_ATTRIBUTES2, 0),
};

// The server's version in the ##.##.#### form SQLGetInfo prescribes.
[[nodiscard]] std::string dbms_version(const std::string& v) {
    std::array<int, 3> part = {};
    const auto* p = v.data();
    const auto* stop = v.data() + v.size();
    for (std::size_t i = 0; i < part.size(); ++i) {
        const auto [end, ec] = std::from_chars(p, stop, part[i]);
        if (ec != std::errc{}) {
            return v;              // not maj.min.patch: report it verbatim
        }
        p = end < stop && *end == '.' ? end + 1 : end;
    }
    return std::format("{:02}.{:02}.{:04}", part[0], part[1], part[2]);
}

// ---- SQLGetFunctions -----------------------------------------------------

constexpr std::array<SQLUSMALLINT, 43> implemented_functions = {
    SQL_API_SQLALLOCHANDLE,   SQL_API_SQLBINDCOL,        SQL_API_SQLBINDPARAMETER,
    SQL_API_SQLCANCEL,        SQL_API_SQLCLOSECURSOR,    SQL_API_SQLCOLATTRIBUTE,
    SQL_API_SQLCOLUMNS,       SQL_API_SQLCONNECT,        SQL_API_SQLDESCRIBECOL,
    SQL_API_SQLDISCONNECT,    SQL_API_SQLDRIVERCONNECT,  SQL_API_SQLENDTRAN,
    SQL_API_SQLEXECDIRECT,    SQL_API_SQLEXECUTE,        SQL_API_SQLFETCH,
    SQL_API_SQLFETCHSCROLL,   SQL_API_SQLFOREIGNKEYS,    SQL_API_SQLFREEHANDLE,
    SQL_API_SQLFREESTMT,      SQL_API_SQLGETCONNECTATTR, SQL_API_SQLGETCURSORNAME,
    SQL_API_SQLGETDATA,       SQL_API_SQLGETDIAGFIELD,   SQL_API_SQLGETDIAGREC,
    SQL_API_SQLGETENVATTR,    SQL_API_SQLGETFUNCTIONS,   SQL_API_SQLGETINFO,
    SQL_API_SQLGETSTMTATTR,   SQL_API_SQLGETTYPEINFO,    SQL_API_SQLMORERESULTS,
    SQL_API_SQLNATIVESQL,     SQL_API_SQLNUMPARAMS,      SQL_API_SQLNUMRESULTCOLS,
    SQL_API_SQLPREPARE,       SQL_API_SQLPRIMARYKEYS,    SQL_API_SQLROWCOUNT,
    SQL_API_SQLSETCONNECTATTR, SQL_API_SQLSETCURSORNAME, SQL_API_SQLSETENVATTR,
    SQL_API_SQLSETSTMTATTR,   SQL_API_SQLSPECIALCOLUMNS, SQL_API_SQLSTATISTICS,
    SQL_API_SQLTABLES,
};

// ---- execute and fetch with status reporting -----------------------------

// SQL_ATTR_PARAMS_PROCESSED_PTR / PARAM_STATUS_PTR: paramset size is fixed
// at 1, so an execute processes one set, successfully or not.
Rc report_params(sqro::Statement* stmt, Rc rc) {
    if (stmt->params_processed_ptr != nullptr) {
        *stmt->params_processed_ptr = (rc == Rc::ok || rc == Rc::ok_info) ? 1 : 0;
    }
    if (stmt->param_status_ptr != nullptr) {
        stmt->param_status_ptr[0] = rc == Rc::error ? SQL_PARAM_ERROR
                                    : rc == Rc::ok_info ? SQL_PARAM_SUCCESS_WITH_INFO
                                                        : SQL_PARAM_SUCCESS;
    }
    return rc;
}

Rc do_fetch(sqro::Statement* stmt) {
    const auto rc = stmt->fetch();
    if (stmt->rows_fetched_ptr != nullptr) {
        *stmt->rows_fetched_ptr = (rc == Rc::ok || rc == Rc::ok_info) ? 1 : 0;
    }
    if (stmt->row_status_ptr != nullptr) {
        stmt->row_status_ptr[0] = rc == Rc::no_data   ? SQL_ROW_NOROW
                                  : rc == Rc::error   ? SQL_ROW_ERROR
                                  : rc == Rc::ok_info ? SQL_ROW_SUCCESS_WITH_INFO
                                                      : SQL_ROW_SUCCESS;
    }
    return rc;
}

// Does an SQLTables type list ask for TABLEs?  The list is comma-separated;
// each entry may be quoted ('TABLE') and blank-padded; "%" means every type.
// Token-wise comparison: "SYSTEM TABLE" must NOT match.
[[nodiscard]] bool wants_tables(std::string_view types) {
    if (types.empty()) {
        return true;
    }
    for (;;) {
        const auto comma = types.find(',');
        auto item = types.substr(0, comma);
        while (!item.empty() && item.front() == ' ') {
            item.remove_prefix(1);
        }
        while (!item.empty() && item.back() == ' ') {
            item.remove_suffix(1);
        }
        if (item.size() >= 2 && item.front() == '\'' && item.back() == '\'') {
            item = item.substr(1, item.size() - 2);
        }
        if (item == "%") {
            return true;
        }
        std::string up(item);
        std::ranges::transform(up, up.begin(),
                               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (up == "TABLE") {
            return true;
        }
        if (comma == std::string_view::npos) {
            return false;
        }
        types = types.substr(comma + 1);
    }
}

// The DiagStack behind a (type, handle) pair; nullptr for a bad combination.
[[nodiscard]] DiagStack* diag_of(SQLSMALLINT type, SQLHANDLE h) {
    if (h == nullptr) {
        return nullptr;
    }
    switch (type) {
    case SQL_HANDLE_ENV:  return &as_env(h)->diag;
    case SQL_HANDLE_DBC:  return &as_dbc(h)->diag;
    case SQL_HANDLE_STMT: return &as_stmt(h)->diag;
    default:              return nullptr;
    }
}

} // namespace

// ==== handle management ==================================================

SQLRETURN SQLAllocHandle(SQLSMALLINT HandleType, SQLHANDLE InputHandle,
                         SQLHANDLE* OutputHandle) {
    if (OutputHandle == nullptr) {
        return SQL_ERROR;
    }
    *OutputHandle = SQL_NULL_HANDLE;
    switch (HandleType) {
    case SQL_HANDLE_ENV: {
        try {
            const std::scoped_lock lock(env_mutex());
            environments().push_back(std::make_unique<sqro::Environment>());
            *OutputHandle = environments().back().get();
            return SQL_SUCCESS;
        } catch (...) {
            return SQL_ERROR;
        }
    }
    case SQL_HANDLE_DBC: {
        auto* env = as_env(InputHandle);
        return guard(env, [&]() {
            // The env's connection vector is the one piece of state two
            // application threads may legitimately touch at once.
            const std::scoped_lock lock(env_mutex());
            auto* dbc = env->create_connection();
            *OutputHandle = dbc;
            return dbc != nullptr ? Rc::ok : Rc::error;
        });
    }
    case SQL_HANDLE_STMT: {
        auto* dbc = as_dbc(InputHandle);
        return guard(dbc, [&]() {
            auto* stmt = dbc->create_statement();
            *OutputHandle = stmt;
            return stmt != nullptr ? Rc::ok : Rc::error;
        });
    }
    default:
        // Explicit descriptors are not supported (implicit ones are all a
        // Core driver needs).
        return SQL_ERROR;
    }
}

SQLRETURN SQLFreeHandle(SQLSMALLINT HandleType, SQLHANDLE Handle) {
    switch (HandleType) {
    case SQL_HANDLE_ENV: {
        auto* env = as_env(Handle);
        return guard(env, [&]() {
            env->diag.clear();
            const std::scoped_lock lock(env_mutex());
            if (env->has_connections()) {
                env->diag.post("HY010", "environment still has connections");
                return Rc::error;
            }
            std::erase_if(environments(), [env](const auto& p) { return p.get() == env; });
            return Rc::ok;
        });
    }
    case SQL_HANDLE_DBC: {
        auto* dbc = as_dbc(Handle);
        return guard(dbc, [&]() {
            dbc->diag.clear();
            if (dbc->connected()) {
                dbc->diag.post("HY010", "connection is still open");
                return Rc::error;
            }
            const std::scoped_lock lock(env_mutex());
            return dbc->env().free_connection(dbc);
        });
    }
    case SQL_HANDLE_STMT: {
        auto* stmt = as_stmt(Handle);
        return guard(stmt, [&]() { return stmt->connection().free_statement(stmt); });
    }
    default:
        return SQL_INVALID_HANDLE;
    }
}

SQLRETURN SQLFreeStmt(SQLHSTMT StatementHandle, SQLUSMALLINT Option) {
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        switch (Option) {
        case SQL_CLOSE:        return stmt->free_close();
        case SQL_UNBIND:       return stmt->unbind_cols();
        case SQL_RESET_PARAMS: return stmt->reset_params();
        case SQL_DROP:         return stmt->connection().free_statement(stmt);
        default:
            stmt->diag.clear();
            stmt->diag.post("HY092", "invalid attribute/option identifier");
            return Rc::error;
        }
    });
}

// ==== connecting =========================================================

SQLRETURN SQLConnect(SQLHDBC ConnectionHandle, SQLCHAR* ServerName, SQLSMALLINT NameLength1,
                     SQLCHAR* UserName, SQLSMALLINT NameLength2,
                     SQLCHAR* Authentication, SQLSMALLINT NameLength3) {
    (void)UserName; (void)NameLength2; (void)Authentication; (void)NameLength3;
    auto* dbc = as_dbc(ConnectionHandle);
    return guard(dbc, [&]() {
        ConnParams p;
        p.dsn = arg_str(ServerName, NameLength1);
        return do_connect(dbc, std::move(p));
    });
}

SQLRETURN SQLDriverConnect(SQLHDBC ConnectionHandle, SQLHWND WindowHandle,
                           SQLCHAR* InConnectionString, SQLSMALLINT StringLength1,
                           SQLCHAR* OutConnectionString, SQLSMALLINT BufferLength,
                           SQLSMALLINT* StringLength2Ptr, SQLUSMALLINT DriverCompletion) {
    (void)WindowHandle; (void)DriverCompletion;   // no prompting: complete or fail
    auto* dbc = as_dbc(ConnectionHandle);
    return guard(dbc, [&]() {
        auto p = parse_conn_str(arg_str(InConnectionString, StringLength1));
        const auto rc = do_connect(dbc, std::move(p));
        if (rc != Rc::ok) {
            return rc;
        }
        // Report the resolved parameters back in canonical form.
        return copy_str(canonical_conn_str(*dbc), OutConnectionString, BufferLength,
                        StringLength2Ptr, dbc->diag);
    });
}

SQLRETURN SQLDisconnect(SQLHDBC ConnectionHandle) {
    auto* dbc = as_dbc(ConnectionHandle);
    return guard(dbc, [&]() { return dbc->disconnect(); });
}

SQLRETURN SQLNativeSql(SQLHDBC ConnectionHandle, SQLCHAR* InStatementText, SQLINTEGER TextLength1,
                       SQLCHAR* OutStatementText, SQLINTEGER BufferLength,
                       SQLINTEGER* TextLength2Ptr) {
    auto* dbc = as_dbc(ConnectionHandle);
    return guard(dbc, [&]() {
        dbc->diag.clear();
        // No escape sequences, no rewriting: the native SQL is the input.
        return copy_str(arg_str(InStatementText, TextLength1), OutStatementText,
                        BufferLength, TextLength2Ptr, dbc->diag);
    });
}

// ==== environment attributes =============================================

SQLRETURN SQLSetEnvAttr(SQLHENV EnvironmentHandle, SQLINTEGER Attribute, SQLPOINTER Value,
                        SQLINTEGER StringLength) {
    (void)StringLength;
    auto* env = as_env(EnvironmentHandle);
    return guard(env, [&]() {
        env->diag.clear();
        switch (Attribute) {
        case SQL_ATTR_ODBC_VERSION:
            env->odbc_version = static_cast<long>(attr_int(Value));
            return Rc::ok;
        case SQL_ATTR_OUTPUT_NTS:
            if (attr_int(Value) != SQL_TRUE) {
                env->diag.post("HYC00", "SQL_ATTR_OUTPUT_NTS = SQL_FALSE is not supported");
                return Rc::error;
            }
            return Rc::ok;
        default:
            env->diag.post("HY092", "invalid attribute/option identifier");
            return Rc::error;
        }
    });
}

SQLRETURN SQLGetEnvAttr(SQLHENV EnvironmentHandle, SQLINTEGER Attribute, SQLPOINTER Value,
                        SQLINTEGER BufferLength, SQLINTEGER* StringLength) {
    (void)BufferLength; (void)StringLength;
    auto* env = as_env(EnvironmentHandle);
    return guard(env, [&]() {
        env->diag.clear();
        switch (Attribute) {
        case SQL_ATTR_ODBC_VERSION:
            put_num(Value, static_cast<SQLINTEGER>(env->odbc_version));
            return Rc::ok;
        case SQL_ATTR_OUTPUT_NTS:
            put_num(Value, static_cast<SQLINTEGER>(SQL_TRUE));
            return Rc::ok;
        default:
            env->diag.post("HY092", "invalid attribute/option identifier");
            return Rc::error;
        }
    });
}

// ==== connection attributes ==============================================

SQLRETURN SQLSetConnectAttr(SQLHDBC ConnectionHandle, SQLINTEGER Attribute, SQLPOINTER Value,
                            SQLINTEGER StringLength) {
    (void)StringLength;
    auto* dbc = as_dbc(ConnectionHandle);
    return guard(dbc, [&]() {
        switch (Attribute) {
        case SQL_ATTR_AUTOCOMMIT:
            return dbc->set_autocommit(attr_int(Value) == SQL_AUTOCOMMIT_ON);
        case SQL_ATTR_LOGIN_TIMEOUT:
        case SQL_ATTR_CONNECTION_TIMEOUT:
            dbc->diag.clear();
            dbc->set_timeout(static_cast<int>(attr_int(Value)));
            return Rc::ok;
        case SQL_ATTR_TXN_ISOLATION:
            dbc->diag.clear();
            if (attr_int(Value) != SQL_TXN_SERIALIZABLE) {
                dbc->diag.post("01S02", "isolation level substituted: serializable");
                return Rc::ok_info;
            }
            return Rc::ok;
        case SQL_ATTR_ACCESS_MODE:
            dbc->diag.clear();
            if (attr_int(Value) != SQL_MODE_READ_WRITE) {
                dbc->diag.post("01S02", "access mode substituted: read-write");
                return Rc::ok_info;
            }
            return Rc::ok;
        case SQL_ATTR_METADATA_ID:
            dbc->diag.clear();
            if (attr_int(Value) != SQL_FALSE) {
                dbc->diag.post("HYC00", "SQL_ATTR_METADATA_ID is not supported");
                return Rc::error;
            }
            return Rc::ok;
        default:
            dbc->diag.clear();
            dbc->diag.post("HY092", "invalid attribute/option identifier");
            return Rc::error;
        }
    });
}

SQLRETURN SQLGetConnectAttr(SQLHDBC ConnectionHandle, SQLINTEGER Attribute, SQLPOINTER Value,
                            SQLINTEGER BufferLength, SQLINTEGER* StringLengthPtr) {
    auto* dbc = as_dbc(ConnectionHandle);
    return guard(dbc, [&]() {
        dbc->diag.clear();
        switch (Attribute) {
        case SQL_ATTR_AUTOCOMMIT:
            put_num(Value, static_cast<SQLUINTEGER>(dbc->autocommit() ? SQL_AUTOCOMMIT_ON
                                                                      : SQL_AUTOCOMMIT_OFF));
            return Rc::ok;
        case SQL_ATTR_TXN_ISOLATION:
            put_num(Value, static_cast<SQLUINTEGER>(SQL_TXN_SERIALIZABLE));
            return Rc::ok;
        case SQL_ATTR_ACCESS_MODE:
            put_num(Value, static_cast<SQLUINTEGER>(SQL_MODE_READ_WRITE));
            return Rc::ok;
        case SQL_ATTR_CONNECTION_DEAD:
            put_num(Value, static_cast<SQLUINTEGER>(dbc->connected() ? SQL_CD_FALSE
                                                                     : SQL_CD_TRUE));
            return Rc::ok;
        case SQL_ATTR_LOGIN_TIMEOUT:
        case SQL_ATTR_CONNECTION_TIMEOUT:
            put_num(Value, static_cast<SQLUINTEGER>(dbc->timeout()));
            return Rc::ok;
        case SQL_ATTR_METADATA_ID:
            put_num(Value, static_cast<SQLUINTEGER>(SQL_FALSE));
            return Rc::ok;
        case SQL_ATTR_CURRENT_CATALOG:
            return copy_str(dbc->dbname(), Value, BufferLength, StringLengthPtr, dbc->diag);
        default:
            dbc->diag.post("HY092", "invalid attribute/option identifier");
            return Rc::error;
        }
    });
}

// ==== statement attributes ===============================================

SQLRETURN SQLSetStmtAttr(SQLHSTMT StatementHandle, SQLINTEGER Attribute, SQLPOINTER Value,
                         SQLINTEGER StringLength) {
    (void)StringLength;
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        stmt->diag.clear();
        // Accept an attribute when the requested value is the only one we
        // do; substitute (01S02) when a different value was asked for.
        const auto only = [&](SQLULEN supported, const char* what) {
            if (attr_int(Value) != supported) {
                stmt->diag.post("01S02", std::format("option value changed: {}", what));
                return Rc::ok_info;
            }
            return Rc::ok;
        };
        switch (Attribute) {
        case SQL_ATTR_QUERY_TIMEOUT:     return only(0, "no query timeout");
        case SQL_ATTR_MAX_ROWS:          return only(0, "no row limit");
        case SQL_ATTR_MAX_LENGTH:        return only(0, "no length limit");
        case SQL_ATTR_ROW_ARRAY_SIZE:    return only(1, "rowset size 1");
        case SQL_ATTR_PARAMSET_SIZE:     return only(1, "paramset size 1");
        case SQL_ATTR_CURSOR_TYPE:       return only(SQL_CURSOR_FORWARD_ONLY, "forward-only cursor");
        case SQL_ATTR_CONCURRENCY:       return only(SQL_CONCUR_READ_ONLY, "read-only concurrency");
        case SQL_ATTR_CURSOR_SCROLLABLE: return only(SQL_NONSCROLLABLE, "non-scrollable cursor");
        case SQL_ATTR_RETRIEVE_DATA:     return only(SQL_RD_ON, "retrieve data on");
        case SQL_ATTR_ROW_BIND_TYPE:     return only(SQL_BIND_BY_COLUMN, "column-wise binding");
        case SQL_ATTR_PARAM_BIND_TYPE:   return only(SQL_PARAM_BIND_BY_COLUMN, "column-wise binding");
        case SQL_ATTR_USE_BOOKMARKS:     return only(SQL_UB_OFF, "bookmarks off");
        case SQL_ATTR_NOSCAN:
            return Rc::ok;             // no escape sequences to scan either way
        case SQL_ATTR_CURSOR_SENSITIVITY:
            if (attr_int(Value) == SQL_SENSITIVE) {
                stmt->diag.post("01S02", "option value changed: insensitive cursor");
                return Rc::ok_info;
            }
            return Rc::ok;
        case SQL_ATTR_ROWS_FETCHED_PTR:
            stmt->rows_fetched_ptr = static_cast<SQLULEN*>(Value);
            return Rc::ok;
        case SQL_ATTR_ROW_STATUS_PTR:
            stmt->row_status_ptr = static_cast<SQLUSMALLINT*>(Value);
            return Rc::ok;
        case SQL_ATTR_PARAMS_PROCESSED_PTR:
            stmt->params_processed_ptr = static_cast<SQLULEN*>(Value);
            return Rc::ok;
        case SQL_ATTR_PARAM_STATUS_PTR:
            stmt->param_status_ptr = static_cast<SQLUSMALLINT*>(Value);
            return Rc::ok;
        case SQL_ATTR_ROW_BIND_OFFSET_PTR:
        case SQL_ATTR_PARAM_BIND_OFFSET_PTR:
            if (Value != nullptr) {
                stmt->diag.post("HYC00", "bind offsets are not supported");
                return Rc::error;
            }
            return Rc::ok;
        default:
            stmt->diag.post("HY092", "invalid attribute/option identifier");
            return Rc::error;
        }
    });
}

SQLRETURN SQLGetStmtAttr(SQLHSTMT StatementHandle, SQLINTEGER Attribute, SQLPOINTER Value,
                         SQLINTEGER BufferLength, SQLINTEGER* StringLength) {
    (void)BufferLength; (void)StringLength;
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        stmt->diag.clear();
        switch (Attribute) {
        case SQL_ATTR_QUERY_TIMEOUT:
        case SQL_ATTR_MAX_ROWS:
        case SQL_ATTR_MAX_LENGTH:
            put_num(Value, static_cast<SQLULEN>(0));
            return Rc::ok;
        case SQL_ATTR_ROW_ARRAY_SIZE:
        case SQL_ATTR_PARAMSET_SIZE:
            put_num(Value, static_cast<SQLULEN>(1));
            return Rc::ok;
        case SQL_ATTR_CURSOR_TYPE:
            put_num(Value, static_cast<SQLULEN>(SQL_CURSOR_FORWARD_ONLY));
            return Rc::ok;
        case SQL_ATTR_CONCURRENCY:
            put_num(Value, static_cast<SQLULEN>(SQL_CONCUR_READ_ONLY));
            return Rc::ok;
        case SQL_ATTR_CURSOR_SCROLLABLE:
            put_num(Value, static_cast<SQLULEN>(SQL_NONSCROLLABLE));
            return Rc::ok;
        case SQL_ATTR_CURSOR_SENSITIVITY:
            put_num(Value, static_cast<SQLULEN>(SQL_INSENSITIVE));
            return Rc::ok;
        case SQL_ATTR_RETRIEVE_DATA:
            put_num(Value, static_cast<SQLULEN>(SQL_RD_ON));
            return Rc::ok;
        case SQL_ATTR_ROW_BIND_TYPE:
            put_num(Value, static_cast<SQLULEN>(SQL_BIND_BY_COLUMN));
            return Rc::ok;
        case SQL_ATTR_USE_BOOKMARKS:
            put_num(Value, static_cast<SQLULEN>(SQL_UB_OFF));
            return Rc::ok;
        case SQL_ATTR_ROW_NUMBER:
            put_num(Value, static_cast<SQLULEN>(stmt->current_row()));
            return Rc::ok;
        case SQL_ATTR_ROWS_FETCHED_PTR:
            put_num(Value, stmt->rows_fetched_ptr);
            return Rc::ok;
        case SQL_ATTR_ROW_STATUS_PTR:
            put_num(Value, stmt->row_status_ptr);
            return Rc::ok;
        case SQL_ATTR_PARAMS_PROCESSED_PTR:
            put_num(Value, stmt->params_processed_ptr);
            return Rc::ok;
        case SQL_ATTR_PARAM_STATUS_PTR:
            put_num(Value, stmt->param_status_ptr);
            return Rc::ok;
        default:
            stmt->diag.post("HY092", "invalid attribute/option identifier");
            return Rc::error;
        }
    });
}

// ==== information ========================================================

SQLRETURN SQLGetInfo(SQLHDBC ConnectionHandle, SQLUSMALLINT InfoType, SQLPOINTER InfoValue,
                     SQLSMALLINT BufferLength, SQLSMALLINT* StringLengthPtr) {
    auto* dbc = as_dbc(ConnectionHandle);
    return guard(dbc, [&]() {
        dbc->diag.clear();
        // The dynamic answers first.
        switch (InfoType) {
        case SQL_DBMS_VER:
            return copy_str(dbms_version(dbc->server_version()), InfoValue, BufferLength,
                            StringLengthPtr, dbc->diag);
        case SQL_DATABASE_NAME:
            // The absolute directory when the server reports one, as the
            // SQLite driver reports its file path: an application asking
            // "which database am I on?" wants somewhere on disk, not a bare
            // basename that several instances could share.  Falls back to
            // the name against a server that predates INFO.
            return copy_str(dbc->dbdir().empty() ? dbc->dbname() : dbc->dbdir(), InfoValue,
                            BufferLength, StringLengthPtr, dbc->diag);
        case SQL_SERVER_NAME:
            return copy_str(dbc->host, InfoValue, BufferLength, StringLengthPtr, dbc->diag);
        case SQL_DATA_SOURCE_NAME:
            return copy_str(dbc->dsn, InfoValue, BufferLength, StringLengthPtr, dbc->diag);
        default:
            break;
        }
        const auto at = std::ranges::find_if(info_table,
                                             [InfoType](const auto& e) { return e.type == InfoType; });
        if (at == info_table.end()) {
            dbc->diag.post("HY096", std::format("information type {} out of range", InfoType));
            return Rc::error;
        }
        switch (at->kind) {
        case InfoKind::str:
            return copy_str(at->s, InfoValue, BufferLength, StringLengthPtr, dbc->diag);
        case InfoKind::u16:
            put_num(InfoValue, static_cast<SQLUSMALLINT>(at->n));
            return Rc::ok;
        case InfoKind::u32:
            put_num(InfoValue, static_cast<SQLUINTEGER>(at->n));
            return Rc::ok;
        }
        return Rc::error;
    });
}

SQLRETURN SQLGetFunctions(SQLHDBC ConnectionHandle, SQLUSMALLINT FunctionId,
                          SQLUSMALLINT* Supported) {
    auto* dbc = as_dbc(ConnectionHandle);
    return guard(dbc, [&]() {
        dbc->diag.clear();
        if (Supported == nullptr) {
            dbc->diag.post("HY009", "null Supported pointer");
            return Rc::error;
        }
        if (FunctionId == SQL_API_ODBC3_ALL_FUNCTIONS) {
            std::fill_n(Supported, SQL_API_ODBC3_ALL_FUNCTIONS_SIZE, SQLUSMALLINT{0});
            for (const auto id : implemented_functions) {
                Supported[id >> 4] = static_cast<SQLUSMALLINT>(Supported[id >> 4]
                                                               | (1U << (id & 0xF)));
            }
            return Rc::ok;
        }
        if (FunctionId == SQL_API_ALL_FUNCTIONS) {
            std::fill_n(Supported, 100, SQLUSMALLINT{SQL_FALSE});
            for (const auto id : implemented_functions) {
                if (id < 100) {
                    Supported[id] = SQL_TRUE;
                }
            }
            return Rc::ok;
        }
        *Supported = std::ranges::contains(implemented_functions, FunctionId) ? SQL_TRUE
                                                                              : SQL_FALSE;
        return Rc::ok;
    });
}

// ==== statement preparation and execution ================================

SQLRETURN SQLPrepare(SQLHSTMT StatementHandle, SQLCHAR* StatementText, SQLINTEGER TextLength) {
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() { return stmt->prepare(arg_str(StatementText, TextLength)); });
}

SQLRETURN SQLExecute(SQLHSTMT StatementHandle) {
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() { return report_params(stmt, stmt->execute()); });
}

SQLRETURN SQLExecDirect(SQLHSTMT StatementHandle, SQLCHAR* StatementText, SQLINTEGER TextLength) {
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        return report_params(stmt, stmt->exec_direct(arg_str(StatementText, TextLength)));
    });
}

SQLRETURN SQLBindParameter(SQLHSTMT StatementHandle, SQLUSMALLINT ParameterNumber,
                           SQLSMALLINT InputOutputType, SQLSMALLINT ValueType,
                           SQLSMALLINT ParameterType, SQLULEN ColumnSize,
                           SQLSMALLINT DecimalDigits, SQLPOINTER ParameterValuePtr,
                           SQLLEN BufferLength, SQLLEN* StrLen_or_IndPtr) {
    (void)ColumnSize; (void)DecimalDigits; (void)BufferLength;
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        stmt->diag.clear();
        if (InputOutputType != SQL_PARAM_INPUT) {
            stmt->diag.post("HYC00", "only input parameters are supported");
            return Rc::error;
        }
        // StrLen_or_IndPtr is a DEFERRED buffer — its value (including a
        // data-at-execution request) is only examined at execute time.
        return stmt->bind_param(ParameterNumber, ValueType, ParameterType,
                                ParameterValuePtr, StrLen_or_IndPtr);
    });
}

SQLRETURN SQLNumParams(SQLHSTMT StatementHandle, SQLSMALLINT* ParameterCountPtr) {
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        stmt->diag.clear();
        if (!stmt->is_prepared()) {
            stmt->diag.post("HY010", "statement is not prepared");
            return Rc::error;
        }
        put_num(ParameterCountPtr, static_cast<SQLSMALLINT>(stmt->num_params()));
        return Rc::ok;
    });
}

SQLRETURN SQLCancel(SQLHSTMT StatementHandle) {
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        stmt->diag.clear();
        return Rc::ok;    // one blocking request at a time: nothing to cancel
    });
}

// ==== results ============================================================

SQLRETURN SQLNumResultCols(SQLHSTMT StatementHandle, SQLSMALLINT* ColumnCountPtr) {
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        stmt->diag.clear();
        put_num(ColumnCountPtr, static_cast<SQLSMALLINT>(stmt->num_result_cols()));
        return Rc::ok;
    });
}

SQLRETURN SQLRowCount(SQLHSTMT StatementHandle, SQLLEN* RowCountPtr) {
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        stmt->diag.clear();
        put_num(RowCountPtr, stmt->row_count());
        return Rc::ok;
    });
}

SQLRETURN SQLDescribeCol(SQLHSTMT StatementHandle, SQLUSMALLINT ColumnNumber,
                         SQLCHAR* ColumnName, SQLSMALLINT BufferLength,
                         SQLSMALLINT* NameLengthPtr, SQLSMALLINT* DataTypePtr,
                         SQLULEN* ColumnSizePtr, SQLSMALLINT* DecimalDigitsPtr,
                         SQLSMALLINT* NullablePtr) {
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        stmt->diag.clear();
        const auto* c = stmt->column(ColumnNumber);
        if (c == nullptr) {
            stmt->diag.post("07009", "invalid descriptor index");
            return Rc::error;
        }
        const auto f = sqro::type_facts(c->type, c->csize);
        put_num(DataTypePtr, f.sql_type);
        put_num(ColumnSizePtr, static_cast<SQLULEN>(f.column_size));
        put_num(DecimalDigitsPtr, SQLSMALLINT{0});
        put_num(NullablePtr, SQLSMALLINT{SQL_NULLABLE});
        return copy_str(c->name, ColumnName, BufferLength, NameLengthPtr, stmt->diag);
    });
}

SQLRETURN SQLColAttribute(SQLHSTMT StatementHandle, SQLUSMALLINT ColumnNumber,
                          SQLUSMALLINT FieldIdentifier, SQLPOINTER CharacterAttribute,
                          SQLSMALLINT BufferLength, SQLSMALLINT* StringLengthPtr,
                          SQLLEN* NumericAttribute) {
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        stmt->diag.clear();
        if (FieldIdentifier == SQL_DESC_COUNT) {
            put_num(NumericAttribute, static_cast<SQLLEN>(stmt->num_result_cols()));
            return Rc::ok;
        }
        const auto* c = stmt->column(ColumnNumber);
        if (c == nullptr) {
            stmt->diag.post("07009", "invalid descriptor index");
            return Rc::error;
        }
        const auto f = sqro::type_facts(c->type, c->csize);
        const auto is_int = c->type == sqrw::ColType::Int;
        const auto text = [&](std::string_view sv) {
            return copy_str(sv, CharacterAttribute, BufferLength, StringLengthPtr, stmt->diag);
        };
        const auto num = [&](SQLLEN v) {
            put_num(NumericAttribute, v);
            return Rc::ok;
        };
        switch (FieldIdentifier) {
        case SQL_DESC_NAME:
        case SQL_DESC_LABEL:
        case SQL_DESC_BASE_COLUMN_NAME:  return text(c->name);
        case SQL_DESC_TYPE_NAME:
        case SQL_DESC_LOCAL_TYPE_NAME:   return text(f.type_name);
        case SQL_DESC_BASE_TABLE_NAME:
        case SQL_DESC_TABLE_NAME:
        case SQL_DESC_CATALOG_NAME:
        case SQL_DESC_SCHEMA_NAME:       return text("");
        case SQL_DESC_LITERAL_PREFIX:
        case SQL_DESC_LITERAL_SUFFIX:    return text(f.is_char ? "'" : "");
        case SQL_DESC_TYPE:
        case SQL_DESC_CONCISE_TYPE:      return num(f.sql_type);
        case SQL_DESC_LENGTH:
        case SQL_DESC_PRECISION:         return num(f.column_size);
        case SQL_DESC_OCTET_LENGTH:      return num(is_int ? 4
                                                   : c->type == sqrw::ColType::Real
                                                       ? 8
                                                       : f.column_size);
        case SQL_DESC_DISPLAY_SIZE:      return num(is_int ? 11
                                                   : c->type == sqrw::ColType::Real
                                                       ? 24
                                                       : f.column_size);
        case SQL_DESC_SCALE:             return num(0);
        case SQL_DESC_NULLABLE:          return num(SQL_NULLABLE);
        case SQL_DESC_UNSIGNED:          return num(f.is_char ? SQL_TRUE : SQL_FALSE);
        case SQL_DESC_SEARCHABLE:        return num(SQL_PRED_BASIC);
        case SQL_DESC_UPDATABLE:         return num(SQL_ATTR_READWRITE_UNKNOWN);
        case SQL_DESC_AUTO_UNIQUE_VALUE: return num(SQL_FALSE);
        case SQL_DESC_CASE_SENSITIVE:    return num(f.is_char ? SQL_TRUE : SQL_FALSE);
        case SQL_DESC_FIXED_PREC_SCALE:  return num(SQL_FALSE);
        case SQL_DESC_NUM_PREC_RADIX:    return num(f.is_char ? 0 : 10);
        case SQL_DESC_UNNAMED:           return num(SQL_NAMED);
        default:
            stmt->diag.post("HY091", "invalid descriptor field identifier");
            return Rc::error;
        }
    });
}

SQLRETURN SQLBindCol(SQLHSTMT StatementHandle, SQLUSMALLINT ColumnNumber, SQLSMALLINT TargetType,
                     SQLPOINTER TargetValue, SQLLEN BufferLength, SQLLEN* StrLen_or_Ind) {
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        // A null TargetValue unbinds the column.
        return stmt->bind_col(ColumnNumber, TargetValue == nullptr ? SQLSMALLINT{0} : TargetType,
                              TargetValue, BufferLength, StrLen_or_Ind);
    });
}

SQLRETURN SQLFetch(SQLHSTMT StatementHandle) {
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() { return do_fetch(stmt); });
}

SQLRETURN SQLFetchScroll(SQLHSTMT StatementHandle, SQLSMALLINT FetchOrientation,
                         SQLLEN FetchOffset) {
    (void)FetchOffset;
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        if (FetchOrientation != SQL_FETCH_NEXT) {
            stmt->diag.clear();
            stmt->diag.post("HY106", "fetch type out of range (forward-only cursor)");
            return Rc::error;
        }
        return do_fetch(stmt);
    });
}

SQLRETURN SQLGetData(SQLHSTMT StatementHandle, SQLUSMALLINT ColumnNumber, SQLSMALLINT TargetType,
                     SQLPOINTER TargetValue, SQLLEN BufferLength, SQLLEN* StrLen_or_Ind) {
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        return stmt->get_data(ColumnNumber, TargetType, TargetValue, BufferLength, StrLen_or_Ind);
    });
}

SQLRETURN SQLMoreResults(SQLHSTMT StatementHandle) {
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        stmt->diag.clear();
        return Rc::no_data;   // one statement, one result
    });
}

SQLRETURN SQLCloseCursor(SQLHSTMT StatementHandle) {
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() { return stmt->close_cursor(); });
}

SQLRETURN SQLGetCursorName(SQLHSTMT StatementHandle, SQLCHAR* CursorName,
                           SQLSMALLINT BufferLength, SQLSMALLINT* NameLengthPtr) {
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        stmt->diag.clear();
        if (stmt->cursor_name.empty()) {
            // Driver-generated names; a plain counter is unique enough for
            // the driver's single-threaded life.
            static unsigned counter = 0;
            stmt->cursor_name = std::format("SQL_CUR{}", counter++);
        }
        return copy_str(stmt->cursor_name, CursorName, BufferLength, NameLengthPtr, stmt->diag);
    });
}

SQLRETURN SQLSetCursorName(SQLHSTMT StatementHandle, SQLCHAR* CursorName, SQLSMALLINT NameLength) {
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        stmt->diag.clear();
        stmt->cursor_name = arg_str(CursorName, NameLength);
        return Rc::ok;
    });
}

// ==== transactions =======================================================

SQLRETURN SQLEndTran(SQLSMALLINT HandleType, SQLHANDLE Handle, SQLSMALLINT CompletionType) {
    if (CompletionType != SQL_COMMIT && CompletionType != SQL_ROLLBACK) {
        if (auto* diag = diag_of(HandleType, Handle)) {
            diag->clear();
            diag->post("HY012", "invalid transaction operation code");
        }
        return Handle == nullptr ? SQL_INVALID_HANDLE : SQL_ERROR;
    }
    const auto commit = CompletionType == SQL_COMMIT;
    switch (HandleType) {
    case SQL_HANDLE_ENV: {
        auto* env = as_env(Handle);
        return guard(env, [&]() { return env->end_tran(commit); });
    }
    case SQL_HANDLE_DBC: {
        auto* dbc = as_dbc(Handle);
        return guard(dbc, [&]() { return dbc->end_tran(commit); });
    }
    default:
        return SQL_INVALID_HANDLE;
    }
}

// ==== catalogue ==========================================================

SQLRETURN SQLTables(SQLHSTMT StatementHandle, SQLCHAR* CatalogName, SQLSMALLINT NameLength1,
                    SQLCHAR* SchemaName, SQLSMALLINT NameLength2, SQLCHAR* TableName,
                    SQLSMALLINT NameLength3, SQLCHAR* TableType, SQLSMALLINT NameLength4) {
    (void)CatalogName; (void)NameLength1; (void)SchemaName; (void)NameLength2;
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        // A type list that excludes TABLE yields the shape with no rows.
        if (!wants_tables(arg_str(TableType, NameLength4))) {
            return stmt->catalog_tables("\x01no-such-table");   // unmatchable filter
        }
        return stmt->catalog_tables(arg_str(TableName, NameLength3));
    });
}

SQLRETURN SQLColumns(SQLHSTMT StatementHandle, SQLCHAR* CatalogName, SQLSMALLINT NameLength1,
                     SQLCHAR* SchemaName, SQLSMALLINT NameLength2, SQLCHAR* TableName,
                     SQLSMALLINT NameLength3, SQLCHAR* ColumnName, SQLSMALLINT NameLength4) {
    (void)CatalogName; (void)NameLength1; (void)SchemaName; (void)NameLength2;
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        return stmt->catalog_columns(arg_str(TableName, NameLength3),
                                     arg_str(ColumnName, NameLength4));
    });
}

SQLRETURN SQLPrimaryKeys(SQLHSTMT StatementHandle, SQLCHAR* CatalogName, SQLSMALLINT NameLength1,
                         SQLCHAR* SchemaName, SQLSMALLINT NameLength2, SQLCHAR* TableName,
                         SQLSMALLINT NameLength3) {
    (void)CatalogName; (void)NameLength1; (void)SchemaName; (void)NameLength2;
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt,
                 [&]() { return stmt->catalog_primary_keys(arg_str(TableName, NameLength3)); });
}

SQLRETURN SQLGetTypeInfo(SQLHSTMT StatementHandle, SQLSMALLINT DataType) {
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() { return stmt->catalog_type_info(DataType); });
}

SQLRETURN SQLSpecialColumns(SQLHSTMT StatementHandle, SQLUSMALLINT IdentifierType,
                            SQLCHAR* CatalogName, SQLSMALLINT NameLength1, SQLCHAR* SchemaName,
                            SQLSMALLINT NameLength2, SQLCHAR* TableName, SQLSMALLINT NameLength3,
                            SQLUSMALLINT Scope, SQLUSMALLINT Nullable) {
    (void)CatalogName; (void)NameLength1; (void)SchemaName; (void)NameLength2;
    (void)Scope; (void)Nullable;
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() {
        return stmt->catalog_special_columns(arg_str(TableName, NameLength3),
                                             IdentifierType == SQL_BEST_ROWID);
    });
}

SQLRETURN SQLStatistics(SQLHSTMT StatementHandle, SQLCHAR* CatalogName, SQLSMALLINT NameLength1,
                        SQLCHAR* SchemaName, SQLSMALLINT NameLength2, SQLCHAR* TableName,
                        SQLSMALLINT NameLength3, SQLUSMALLINT Unique, SQLUSMALLINT Reserved) {
    (void)CatalogName; (void)NameLength1; (void)SchemaName; (void)NameLength2;
    (void)Unique; (void)Reserved;    // the only index is unique either way
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() { return stmt->catalog_statistics(arg_str(TableName, NameLength3)); });
}

SQLRETURN SQLForeignKeys(SQLHSTMT StatementHandle, SQLCHAR* PKCatalogName, SQLSMALLINT NameLength1,
                         SQLCHAR* PKSchemaName, SQLSMALLINT NameLength2, SQLCHAR* PKTableName,
                         SQLSMALLINT NameLength3, SQLCHAR* FKCatalogName, SQLSMALLINT NameLength4,
                         SQLCHAR* FKSchemaName, SQLSMALLINT NameLength5, SQLCHAR* FKTableName,
                         SQLSMALLINT NameLength6) {
    (void)PKCatalogName; (void)NameLength1; (void)PKSchemaName; (void)NameLength2;
    (void)PKTableName; (void)NameLength3; (void)FKCatalogName; (void)NameLength4;
    (void)FKSchemaName; (void)NameLength5; (void)FKTableName; (void)NameLength6;
    auto* stmt = as_stmt(StatementHandle);
    return guard(stmt, [&]() { return stmt->catalog_foreign_keys(); });
}

// ==== diagnostics ========================================================
// These do NOT clear the diagnostic stack they read.

SQLRETURN SQLGetDiagRec(SQLSMALLINT HandleType, SQLHANDLE Handle, SQLSMALLINT RecNumber,
                        SQLCHAR* SQLState, SQLINTEGER* NativeErrorPtr, SQLCHAR* MessageText,
                        SQLSMALLINT BufferLength, SQLSMALLINT* TextLengthPtr) {
    auto* diag = diag_of(HandleType, Handle);
    if (diag == nullptr) {
        return SQL_INVALID_HANDLE;
    }
    if (RecNumber < 1) {
        return SQL_ERROR;
    }
    const auto* rec = diag->record(static_cast<std::size_t>(RecNumber));
    if (rec == nullptr) {
        return SQL_NO_DATA;
    }
    if (SQLState != nullptr) {
        // Five characters plus the terminator: the buffer is six bytes by
        // specification.
        std::memcpy(SQLState, rec->sqlstate.c_str(), 6);
    }
    if (NativeErrorPtr != nullptr) {
        *NativeErrorPtr = rec->native;
    }
    if (TextLengthPtr != nullptr) {
        *TextLengthPtr = static_cast<SQLSMALLINT>(rec->message.size());
    }
    if (MessageText != nullptr && BufferLength > 0) {
        auto* out = reinterpret_cast<char*>(MessageText);
        const auto ncopy = std::min(rec->message.size(),
                                    static_cast<std::size_t>(BufferLength) - 1);
        std::memcpy(out, rec->message.data(), ncopy);
        out[ncopy] = '\0';
        if (ncopy < rec->message.size()) {
            return SQL_SUCCESS_WITH_INFO;
        }
    }
    return SQL_SUCCESS;
}

SQLRETURN SQLGetDiagField(SQLSMALLINT HandleType, SQLHANDLE Handle, SQLSMALLINT RecNumber,
                          SQLSMALLINT DiagIdentifier, SQLPOINTER DiagInfo,
                          SQLSMALLINT BufferLength, SQLSMALLINT* StringLengthPtr) {
    auto* diag = diag_of(HandleType, Handle);
    if (diag == nullptr) {
        return SQL_INVALID_HANDLE;
    }
    // Header fields (RecNumber ignored).
    switch (DiagIdentifier) {
    case SQL_DIAG_NUMBER:
        put_num(DiagInfo, static_cast<SQLINTEGER>(diag->size()));
        return SQL_SUCCESS;
    case SQL_DIAG_RETURNCODE:
        put_num(DiagInfo, static_cast<SQLRETURN>(diag->return_code()));
        return SQL_SUCCESS;
    case SQL_DIAG_ROW_COUNT:
    case SQL_DIAG_CURSOR_ROW_COUNT:
        // Statement-only headers; both are the materialised count here
        // (affected rows for DML, result-set size for a cursor).
        if (HandleType != SQL_HANDLE_STMT) {
            return SQL_ERROR;
        }
        put_num(DiagInfo, as_stmt(Handle)->row_count());
        return SQL_SUCCESS;
    case SQL_DIAG_DYNAMIC_FUNCTION:
        if (HandleType != SQL_HANDLE_STMT) {
            return SQL_ERROR;
        }
        {
            DiagStack scratch;
            const auto rc = copy_str("", DiagInfo, BufferLength, StringLengthPtr, scratch);
            return rc == Rc::error ? SQL_ERROR : SQL_SUCCESS;
        }
    case SQL_DIAG_DYNAMIC_FUNCTION_CODE:
        if (HandleType != SQL_HANDLE_STMT) {
            return SQL_ERROR;
        }
        put_num(DiagInfo, static_cast<SQLINTEGER>(SQL_DIAG_UNKNOWN_STATEMENT));
        return SQL_SUCCESS;
    default:
        break;
    }
    // Record fields.
    const auto* rec = RecNumber >= 1 ? diag->record(static_cast<std::size_t>(RecNumber)) : nullptr;
    if (rec == nullptr) {
        return SQL_NO_DATA;
    }
    const auto text = [&](std::string_view sv) {
        DiagStack scratch;    // copy_str posts truncation here; the field API has no warning channel
        const auto rc = copy_str(sv, DiagInfo, BufferLength, StringLengthPtr, scratch);
        return rc == Rc::error ? SQL_ERROR
               : rc == Rc::ok_info ? SQL_SUCCESS_WITH_INFO
                                   : SQL_SUCCESS;
    };
    switch (DiagIdentifier) {
    case SQL_DIAG_SQLSTATE:        return text(rec->sqlstate);
    case SQL_DIAG_MESSAGE_TEXT:    return text(rec->message);
    case SQL_DIAG_CLASS_ORIGIN:
        return text(rec->sqlstate.starts_with("IM") ? "ODBC 3.0" : "ISO 9075");
    case SQL_DIAG_SUBCLASS_ORIGIN: return text("ODBC 3.0");
    case SQL_DIAG_CONNECTION_NAME:
    case SQL_DIAG_SERVER_NAME:     return text("");
    case SQL_DIAG_NATIVE:
        put_num(DiagInfo, static_cast<SQLINTEGER>(rec->native));
        return SQL_SUCCESS;
    default:
        return SQL_ERROR;
    }
}
