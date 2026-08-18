// model.cxx — Environment / Connection / Statement implementation (layer 2).
// The catalogue result-set builders live in catalog.cxx.

#include "model.hxx"

#include <algorithm>
#include <array>
#include <cmath>
#include <charconv>
#include <cstring>
#include <format>
#include <limits>
#include <utility>

namespace sqro {

namespace {

    // ---- fetch-time output conversion -----------------------------------

    // Resolve SQL_C_DEFAULT to the natural C type of the source column.
    [[nodiscard]] SQLSMALLINT resolve_ctype(SQLSMALLINT ctype, sqrw::ColType src) {
        if (ctype != SQL_C_DEFAULT) {
            return ctype;
        }
        switch (src) {
        case sqrw::ColType::Int:  return SQL_C_SLONG;
        case sqrw::ColType::Real: return SQL_C_DOUBLE;
        case sqrw::ColType::Char:
        case sqrw::ColType::Text: return SQL_C_CHAR;
        }
        return SQL_C_CHAR;
    }

    // The text form of a cell (for SQL_C_CHAR targets): numerics via the
    // wire layer's shortest-round-trip formatters.
    [[nodiscard]] std::string cell_text(const sqrw::Cell& cell) {
        if (const auto* i = std::get_if<std::int32_t>(&cell)) {
            return sqrw::sql_int(*i);
        }
        if (const auto* d = std::get_if<double>(&cell)) {
            return sqrw::sql_real(*d);
        }
        if (const auto* s = std::get_if<std::string>(&cell)) {
            return *s;
        }
        return {};
    }

    // Parse a whole string as a double (from_chars: locale-free).
    [[nodiscard]] std::optional<double> parse_real(std::string_view s) {
        double v = 0;
        const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        if (ec != std::errc{} || p != s.data() + s.size()) {
            return std::nullopt;
        }
        return v;
    }

    // A cell's numeric value as a double, if it has one (22018 otherwise).
    [[nodiscard]] std::optional<double> cell_number(const sqrw::Cell& cell) {
        if (const auto* i = std::get_if<std::int32_t>(&cell)) {
            return static_cast<double>(*i);
        }
        if (const auto* d = std::get_if<double>(&cell)) {
            return *d;
        }
        if (const auto* s = std::get_if<std::string>(&cell)) {
            return parse_real(*s);
        }
        return std::nullopt;
    }

    // Store an integral target read from `v`, diagnosing range (22003) and
    // discarded fractions (01S07).  The bounds compare as long double: a
    // 64-bit target's max is not representable in a double (it rounds up to
    // 2^63), so double bounds would wave through an out-of-range value.
    template <class Int>
    [[nodiscard]] Rc store_integral(double v, void* buf, SQLLEN* ind, DiagStack& diag) {
        constexpr auto lo = static_cast<long double>(std::numeric_limits<Int>::min());
        constexpr auto hi = static_cast<long double>(std::numeric_limits<Int>::max());
        if (!std::isfinite(v) || static_cast<long double>(v) < lo
            || static_cast<long double>(v) > hi) {
            diag.post("22003", "numeric value out of range");
            return Rc::error;
        }
        const auto whole = std::trunc(v);
        *static_cast<Int*>(buf) = static_cast<Int>(whole);
        if (ind != nullptr) {
            *ind = sizeof(Int);
        }
        if (whole != v) {
            diag.post("01S07", "fractional truncation");
            return Rc::ok_info;
        }
        return Rc::ok;
    }

    // The full cell -> integral-target path: numeric value (22018 when the
    // cell has none), then range-checked store.
    template <class Int>
    [[nodiscard]] Rc store_int_cell(const sqrw::Cell& cell, void* buf, SQLLEN* ind,
                                    DiagStack& diag) {
        const auto v = cell_number(cell);
        if (!v) {
            diag.post("22018", "invalid character value for cast");
            return Rc::error;
        }
        return store_integral<Int>(*v, buf, ind, diag);
    }

    // Copy one cell into an application buffer with conversion.  For
    // SQL_C_CHAR targets `offset` (when non-null) supports SQLGetData's
    // chunked retrieval: copying resumes there and it advances past the
    // bytes delivered; `text` (when non-null) is the caller's cached
    // rendering of the cell, so chunked retrieval of a large value does not
    // re-render it per call.  Returns ok / ok_info (with a diagnostic) /
    // error.
    [[nodiscard]] Rc convert_out(const sqrw::Cell& cell, const sqrw::Column& meta,
                                 SQLSMALLINT ctype_in, void* buf, SQLLEN buflen,
                                 SQLLEN* ind, std::size_t* offset, DiagStack& diag,
                                 const std::string* text = nullptr) {
        if (std::holds_alternative<std::monostate>(cell)) {
            if (ind == nullptr) {
                diag.post("22002", "indicator variable required but not supplied");
                return Rc::error;
            }
            *ind = SQL_NULL_DATA;
            return Rc::ok;
        }
        const auto ctype = resolve_ctype(ctype_in, meta.type);
        switch (ctype) {
        case SQL_C_LONG:
        case SQL_C_SLONG:
            return store_int_cell<SQLINTEGER>(cell, buf, ind, diag);
        case SQL_C_ULONG:
            return store_int_cell<SQLUINTEGER>(cell, buf, ind, diag);
        case SQL_C_SHORT:
        case SQL_C_SSHORT:
            return store_int_cell<SQLSMALLINT>(cell, buf, ind, diag);
        case SQL_C_USHORT:
            return store_int_cell<SQLUSMALLINT>(cell, buf, ind, diag);
        case SQL_C_SBIGINT:
            return store_int_cell<SQLBIGINT>(cell, buf, ind, diag);
        case SQL_C_UBIGINT:
            return store_int_cell<SQLUBIGINT>(cell, buf, ind, diag);
        case SQL_C_TINYINT:
        case SQL_C_STINYINT:
            return store_int_cell<SQLSCHAR>(cell, buf, ind, diag);
        case SQL_C_UTINYINT:
            return store_int_cell<SQLCHAR>(cell, buf, ind, diag);
        case SQL_C_BIT: {
            const auto v = cell_number(cell);
            if (!v) {
                diag.post("22018", "invalid character value for cast");
                return Rc::error;
            }
            if (!std::isfinite(*v) || *v < 0 || *v >= 2) {
                diag.post("22003", "numeric value out of range");
                return Rc::error;
            }
            const auto whole = std::trunc(*v);
            *static_cast<unsigned char*>(buf) = static_cast<unsigned char>(whole);
            if (ind != nullptr) {
                *ind = 1;
            }
            if (whole != *v) {
                diag.post("01S07", "fractional truncation");
                return Rc::ok_info;
            }
            return Rc::ok;
        }
        case SQL_C_DOUBLE: {
            const auto v = cell_number(cell);
            if (!v) {
                diag.post("22018", "invalid character value for cast");
                return Rc::error;
            }
            *static_cast<double*>(buf) = *v;
            if (ind != nullptr) {
                *ind = sizeof(double);
            }
            return Rc::ok;
        }
        case SQL_C_FLOAT: {
            const auto v = cell_number(cell);
            if (!v) {
                diag.post("22018", "invalid character value for cast");
                return Rc::error;
            }
            const auto f = static_cast<float>(*v);
            if (std::isfinite(*v) && !std::isfinite(f)) {
                diag.post("22003", "numeric value out of range");
                return Rc::error;
            }
            *static_cast<float*>(buf) = f;
            if (ind != nullptr) {
                *ind = sizeof(float);
            }
            return Rc::ok;
        }
        case SQL_C_CHAR: {
            if (buflen < 1) {
                diag.post("HY090", "invalid string or buffer length");
                return Rc::error;
            }
            std::string own;
            if (text == nullptr) {
                own = cell_text(cell);
            }
            const std::string& sv = text != nullptr ? *text : own;
            const auto from = offset != nullptr ? std::min(*offset, sv.size()) : 0;
            const auto avail = sv.size() - from;
            const auto room = static_cast<std::size_t>(buflen) - 1;
            const auto ncopy = std::min(avail, room);
            std::memcpy(buf, sv.data() + from, ncopy);
            static_cast<char*>(buf)[ncopy] = '\0';
            if (ind != nullptr) {
                *ind = static_cast<SQLLEN>(avail);   // length remaining before this copy
            }
            if (offset != nullptr) {
                *offset = from + ncopy;
            }
            if (ncopy < avail) {
                diag.post("01004", "string data, right truncated");
                return Rc::ok_info;
            }
            return Rc::ok;
        }
        default:
            diag.post("07006", std::format("restricted data type attribute violation (C type {})", ctype));
            return Rc::error;
        }
    }

    [[nodiscard]] bool char_like(SQLSMALLINT stype) {
        return stype == SQL_CHAR || stype == SQL_VARCHAR || stype == SQL_LONGVARCHAR
            || stype == SQL_WCHAR || stype == SQL_WVARCHAR || stype == SQL_WLONGVARCHAR;
    }

    [[nodiscard]] Rc worst(Rc a, Rc b) {
        return static_cast<int>(a) > static_cast<int>(b) ? a : b;
    }

} // namespace

// ==== Environment ========================================================

Environment::Environment() = default;
Environment::~Environment() = default;
Connection::Connection(Environment& env) : env_(&env) {}
Connection::~Connection() = default;

Connection* Environment::create_connection() {
    diag.clear();
    connections_.push_back(std::make_unique<Connection>(*this));
    return connections_.back().get();
}

Rc Environment::free_connection(Connection* conn) {
    diag.clear();
    const auto at = std::ranges::find_if(connections_,
                                         [conn](const auto& p) { return p.get() == conn; });
    if (at == connections_.end()) {
        return Rc::invalid;
    }
    if ((*at)->connected()) {
        diag.post("HY010", "connection is still open");
        return Rc::error;
    }
    connections_.erase(at);
    return Rc::ok;
}

Rc Environment::end_tran(bool commit) {
    diag.clear();
    auto rc = Rc::ok;
    for (const auto& conn : connections_) {
        if (conn->connected() && conn->end_tran(commit) == Rc::error) {
            diag.post("HY000", "transaction completion failed on a connection");
            rc = Rc::error;
        }
    }
    return rc;
}

// ==== Connection =========================================================

Rc Connection::connect(std::string_view host, int port) {
    diag.clear();
    if (wire_) {
        diag.post("08002", "connection name in use (already connected)");
        return Rc::error;
    }
    auto w = sqrw::Connection::open(host, port, "sqr_odbc", timeout_);
    if (!w) {
        // connect-time failures are 08001 regardless of the wire error kind
        diag.post("08001", w.error().message, w.error().stat);
        return Rc::error;
    }
    wire_ = std::move(*w);
    return Rc::ok;
}

Rc Connection::disconnect() {
    diag.clear();
    if (!wire_) {
        diag.post("08003", "connection not open");
        return Rc::error;
    }
    if (txn_open_) {
        diag.post("25000", "invalid transaction state (transaction still open)");
        return Rc::error;
    }
    statements_.clear();       // ODBC: disconnect frees the connection's statements
    wire_->quit();
    wire_.reset();
    return Rc::ok;
}

const std::string& Connection::dbname() const {
    static const std::string empty;
    return wire_ ? wire_->dbname() : empty;
}

const std::string& Connection::dbdir() const {
    static const std::string empty;
    return wire_ ? wire_->dbdir() : empty;
}

std::string Connection::dbrealdir() const {
    return wire_ ? wire_->info("realdir") : std::string{};
}

const std::string& Connection::server_version() const {
    static const std::string empty;
    return wire_ ? wire_->server_version() : empty;
}

void Connection::set_timeout(int seconds) {
    timeout_ = seconds > 0 ? seconds : 0;
    if (wire_) {
        wire_->set_timeout(timeout_);
    }
}

Rc Connection::set_autocommit(bool on) {
    diag.clear();
    if (on && txn_open_) {
        if (const auto rc = end_tran(true); rc != Rc::ok) {
            return rc;
        }
    }
    autocommit_ = on;
    return Rc::ok;
}

Rc Connection::end_tran(bool commit) {
    diag.clear();
    if (!wire_) {
        diag.post("08003", "connection not open");
        return Rc::error;
    }
    if (!txn_open_) {
        return Rc::ok;         // nothing in flight: a no-op success, per spec
    }
    const auto reply = wire_->exec(commit ? "COMMIT" : "ROLLBACK");
    if (!reply) {
        diag.post(reply.error());
        return Rc::error;
    }
    txn_open_ = false;
    return Rc::ok;
}

sqrw::Result<sqrw::None> Connection::ensure_txn() {
    if (autocommit_ || txn_open_) {
        return sqrw::None{};
    }
    auto reply = wire_->exec("BEGIN");
    if (!reply) {
        return std::unexpected(reply.error());
    }
    txn_open_ = true;
    return sqrw::None{};
}

Statement* Connection::create_statement() {
    diag.clear();
    if (!wire_) {
        diag.post("08003", "connection not open");
        return nullptr;
    }
    statements_.push_back(std::make_unique<Statement>(*this));
    return statements_.back().get();
}

Rc Connection::free_statement(Statement* stmt) {
    diag.clear();
    const auto at = std::ranges::find_if(statements_,
                                         [stmt](const auto& p) { return p.get() == stmt; });
    if (at == statements_.end()) {
        return Rc::invalid;
    }
    statements_.erase(at);
    return Rc::ok;
}

// ==== Statement ==========================================================

Statement::State Statement::state() const {
    if (cursor_open_) {
        return State::cursor;
    }
    // A synthetic (catalogue) result set sets executed_ without any prepared
    // statement; once its cursor closes the statement is back to initial —
    // the executed state (which permits re-execution) needs a plan to run.
    if (executed_ && prepared_) {
        return State::executed;
    }
    if (prepared_) {
        return State::prepared;
    }
    return State::initial;
}

// The statement-legality table (InSet style): rows are operations, columns
// are states; an entry is nullptr when the combination is legal, otherwise
// the SQLSTATE it raises.
bool Statement::gate(Op op) {
    static constexpr std::array<std::array<const char*, 4>, 5> table = {{
        //  initial   prepared  executed  cursor
        {{ nullptr,  nullptr,  nullptr,  "24000" }},   // prepare
        {{ "HY010",  nullptr,  nullptr,  "24000" }},   // execute
        {{ "24000",  "24000",  "24000",  nullptr }},   // fetch
        {{ "HY010",  "HY010",  "HY010",  nullptr }},   // get_data
        {{ "24000",  "24000",  "24000",  nullptr }},   // close_cursor
    }};
    const auto* sqlstate = table[static_cast<std::size_t>(op)][static_cast<std::size_t>(state())];
    if (sqlstate == nullptr) {
        return true;
    }
    diag.post(sqlstate, std::string_view(sqlstate) == "24000"
                            ? "invalid cursor state"
                            : "function called out of sequence");
    return false;
}

Rc Statement::prepare(std::string_view sql) {
    diag.clear();
    if (!gate(Op::prepare)) {
        return Rc::error;
    }
    // Split at '?' parameter markers outside quoted strings.  A doubled ''
    // inside a literal toggles in_quote twice, so it needs no special case.
    segments_.clear();
    std::string cur;
    bool in_quote = false;
    for (const auto c : sql) {
        if (c == '\'') {
            in_quote = !in_quote;
            cur.push_back(c);
        } else if (c == '?' && !in_quote) {
            segments_.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    segments_.push_back(std::move(cur));
    prepared_ = true;
    executed_ = false;
    rs_.reset();
    affected_ = 0;
    row_ = 0;
    return Rc::ok;
}

std::optional<std::string> Statement::format_param(int number) {
    const auto& p = params_[static_cast<std::size_t>(number) - 1];
    // The indicator is a DEFERRED buffer: its value is only meaningful here,
    // at execute time — never at bind time.
    if (p.ind != nullptr) {
        if (*p.ind == SQL_NULL_DATA) {
            return std::string("NULL");
        }
        if (*p.ind == SQL_DATA_AT_EXEC || *p.ind <= SQL_LEN_DATA_AT_EXEC_OFFSET) {
            diag.post("HYC00", "data-at-execution parameters are not supported");
            return std::nullopt;
        }
        if (*p.ind < 0 && *p.ind != SQL_NTS) {
            diag.post("HY090", std::format("invalid parameter length {}", *p.ind));
            return std::nullopt;
        }
    }
    if (p.value == nullptr) {
        diag.post("HY009", "bound parameter buffer is a null pointer");
        return std::nullopt;
    }
    std::string text;
    switch (p.ctype) {
    case SQL_C_LONG:
    case SQL_C_SLONG:
        text = sqrw::sql_int(*static_cast<const SQLINTEGER*>(p.value));
        break;
    case SQL_C_ULONG:
        text = sqrw::sql_int(*static_cast<const SQLUINTEGER*>(p.value));
        break;
    case SQL_C_SHORT:
    case SQL_C_SSHORT:
        text = sqrw::sql_int(*static_cast<const SQLSMALLINT*>(p.value));
        break;
    case SQL_C_USHORT:
        text = sqrw::sql_int(*static_cast<const SQLUSMALLINT*>(p.value));
        break;
    case SQL_C_SBIGINT:
        text = sqrw::sql_int(*static_cast<const SQLBIGINT*>(p.value));
        break;
    case SQL_C_UBIGINT:
        text = std::format("{}", *static_cast<const SQLUBIGINT*>(p.value));
        break;
    case SQL_C_TINYINT:
    case SQL_C_STINYINT:
        text = sqrw::sql_int(*static_cast<const SQLSCHAR*>(p.value));
        break;
    case SQL_C_UTINYINT:
        text = sqrw::sql_int(*static_cast<const SQLCHAR*>(p.value));
        break;
    case SQL_C_BIT:
        text = *static_cast<const unsigned char*>(p.value) != 0 ? "1" : "0";
        break;
    case SQL_C_DOUBLE:
        text = sqrw::sql_real(*static_cast<const double*>(p.value));
        break;
    case SQL_C_FLOAT:
        text = sqrw::sql_real(static_cast<double>(*static_cast<const float*>(p.value)));
        break;
    case SQL_C_CHAR: {
        const auto* s = static_cast<const char*>(p.value);
        const auto len = (p.ind == nullptr || *p.ind == SQL_NTS)
                             ? std::strlen(s)
                             : static_cast<std::size_t>(*p.ind);
        text.assign(s, len);
        break;
    }
    default:
        diag.post("HY003", std::format("unsupported parameter C type {}", p.ctype));
        return std::nullopt;
    }
    // The SQL type decides quoting: character targets get a quoted literal
    // (whatever the C source type), numeric targets get the bare text.
    if (char_like(p.stype)) {
        return sqrw::sql_quote(text);
    }
    return text;
}

Rc Statement::execute() {
    diag.clear();
    if (!gate(Op::execute)) {
        return Rc::error;
    }
    const auto nparams = segments_.size() - 1;
    std::string sql = segments_[0];
    for (std::size_t k = 1; k <= nparams; ++k) {
        if (k > params_.size() || !params_[k - 1].bound) {
            diag.post("07002", std::format("parameter {} is not bound", k));
            return Rc::error;
        }
        auto lit = format_param(static_cast<int>(k));
        if (!lit) {
            return Rc::error;      // format_param posted the diagnostic
        }
        sql += *lit;
        sql += segments_[k];
    }
    if (auto txn = conn_->ensure_txn(); !txn) {
        diag.post(txn.error());
        return Rc::error;
    }
    return run(sql);
}

Rc Statement::exec_direct(std::string_view sql) {
    if (const auto rc = prepare(sql); rc != Rc::ok) {
        return rc;
    }
    return execute();
}

Rc Statement::run(std::string_view sql) {
    auto* wire = conn_->wire();
    if (wire == nullptr) {
        diag.post("08003", "connection not open");
        return Rc::error;
    }
    auto reply = wire->exec(sql);
    if (!reply) {
        diag.post(reply.error());
        return Rc::error;
    }
    executed_ = true;
    rs_.reset();
    cursor_open_ = false;
    row_ = 0;
    affected_ = 0;
    getdata_col_ = 0;
    getdata_off_ = 0;
    getdata_done_ = false;
    getdata_text_.clear();
    if (auto* count = std::get_if<sqrw::Count>(&*reply)) {
        affected_ = count->n;
    } else if (auto* rows = std::get_if<sqrw::RowSet>(&*reply)) {
        affected_ = static_cast<SQLLEN>(rows->rows.size());
        rs_ = std::move(*rows);
        cursor_open_ = true;
    }
    return Rc::ok;
}

Rc Statement::bind_param(int number, SQLSMALLINT ctype, SQLSMALLINT stype,
                         const void* value, const SQLLEN* ind) {
    diag.clear();
    if (number < 1) {
        diag.post("07009", "invalid parameter number");
        return Rc::error;
    }
    if (params_.size() < static_cast<std::size_t>(number)) {
        params_.resize(static_cast<std::size_t>(number));
    }
    params_[static_cast<std::size_t>(number) - 1] =
        {.bound = true, .ctype = ctype, .stype = stype, .value = value, .ind = ind};
    return Rc::ok;
}

Rc Statement::reset_params() {
    diag.clear();
    params_.clear();
    return Rc::ok;
}

int Statement::num_params() const {
    return prepared_ ? static_cast<int>(segments_.size()) - 1 : 0;
}

int Statement::num_result_cols() const {
    return rs_ ? static_cast<int>(rs_->columns.size()) : 0;
}

const sqrw::Column* Statement::column(int i) const {
    if (!rs_ || i < 1 || static_cast<std::size_t>(i) > rs_->columns.size()) {
        return nullptr;
    }
    return &rs_->columns[static_cast<std::size_t>(i) - 1];
}

Rc Statement::bind_col(int i, SQLSMALLINT ctype, void* buf, SQLLEN buflen, SQLLEN* ind) {
    diag.clear();
    if (i < 1) {
        diag.post("07009", "invalid descriptor index");
        return Rc::error;
    }
    if (cols_.size() < static_cast<std::size_t>(i)) {
        cols_.resize(static_cast<std::size_t>(i));
    }
    auto& slot = cols_[static_cast<std::size_t>(i) - 1];
    if (buf == nullptr) {
        slot = {};             // SQLBindCol with a null target unbinds
    } else {
        slot = {.ctype = ctype, .buf = buf, .buflen = buflen, .ind = ind};
    }
    return Rc::ok;
}

Rc Statement::unbind_cols() {
    diag.clear();
    cols_.clear();
    return Rc::ok;
}

Rc Statement::fetch() {
    diag.clear();
    if (!gate(Op::fetch)) {
        return Rc::error;
    }
    const auto& rows = rs_->rows;
    if (row_ >= rows.size()) {
        return Rc::no_data;
    }
    const auto& row = rows[row_];
    auto rc = Rc::ok;
    for (std::size_t j = 0; j < cols_.size() && j < row.size(); ++j) {
        const auto& b = cols_[j];
        if (b.buf == nullptr) {
            continue;
        }
        rc = worst(rc, convert_out(row[j], rs_->columns[j], b.ctype,
                                   b.buf, b.buflen, b.ind, nullptr, diag));
    }
    ++row_;
    getdata_col_ = 0;
    getdata_off_ = 0;
    getdata_done_ = false;
    getdata_text_.clear();
    return rc;
}

Rc Statement::get_data(int i, SQLSMALLINT ctype, void* buf, SQLLEN buflen, SQLLEN* ind) {
    diag.clear();
    if (!gate(Op::get_data)) {
        return Rc::error;
    }
    if (row_ == 0) {
        diag.post("HY010", "no row has been fetched");
        return Rc::error;
    }
    if (i < 1 || static_cast<std::size_t>(i) > rs_->columns.size()) {
        diag.post("07009", "invalid descriptor index");
        return Rc::error;
    }
    if (buf == nullptr) {
        diag.post("HY009", "invalid use of a null pointer");
        return Rc::error;
    }
    if (getdata_col_ == i && getdata_done_) {
        return Rc::no_data;    // this column's data is already fully delivered
    }
    if (getdata_col_ != i) {
        getdata_col_ = i;
        getdata_off_ = 0;
        getdata_done_ = false;
        getdata_text_.clear();
    }
    const auto& cell = rs_->rows[row_ - 1][static_cast<std::size_t>(i) - 1];
    const auto& meta = rs_->columns[static_cast<std::size_t>(i) - 1];
    const auto char_target = resolve_ctype(ctype, meta.type) == SQL_C_CHAR;
    const auto is_null = std::holds_alternative<std::monostate>(cell);
    if (char_target && !is_null) {
        // A 1-byte buffer only ever holds the terminator: no chunk could
        // make progress and a caller's retrieval loop would never end.
        if (buflen < 2) {
            diag.post("HY090", "invalid string or buffer length");
            return Rc::error;
        }
        if (getdata_off_ == 0) {
            getdata_text_ = cell_text(cell);   // rendered once per column
        }
    }
    const auto rc = convert_out(cell, meta, ctype, buf, buflen, ind, &getdata_off_, diag,
                                char_target && !is_null ? &getdata_text_ : nullptr);
    // The column is fully delivered on success — except a truncated
    // character chunk (ok_info on a char target), which continues next call.
    if (rc == Rc::ok || (rc == Rc::ok_info && !char_target)) {
        getdata_done_ = true;
    }
    return rc;
}

Rc Statement::close_cursor() {
    diag.clear();
    if (!gate(Op::close_cursor)) {
        return Rc::error;
    }
    rs_.reset();
    cursor_open_ = false;
    row_ = 0;
    return Rc::ok;
}

Rc Statement::free_close() {
    diag.clear();
    rs_.reset();
    cursor_open_ = false;
    row_ = 0;
    return Rc::ok;
}

Rc Statement::open_synthetic(sqrw::RowSet rs) {
    if (cursor_open_) {
        diag.post("24000", "invalid cursor state");
        return Rc::error;
    }
    affected_ = static_cast<SQLLEN>(rs.rows.size());
    rs_ = std::move(rs);
    cursor_open_ = true;
    executed_ = true;
    row_ = 0;
    getdata_col_ = 0;
    getdata_off_ = 0;
    getdata_done_ = false;
    getdata_text_.clear();
    return Rc::ok;
}

} // namespace sqro
