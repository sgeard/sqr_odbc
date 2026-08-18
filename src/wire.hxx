// wire.hxx — sqrd wire-protocol client: the ODBC-free bottom layer of the
// sqr ODBC driver.
//
// Speaks protocol v1 (sqr/reports/DESIGN-wire-protocol.md): printable header
// lines + byte-counted payloads over a blocking TCP socket, binary
// little-endian INT/REAL cells.  Nothing in this layer knows ODBC exists;
// its tests double as protocol conformance tests against a live sqrd.
//
// Conventions:
//  - Pure value semantics: cells, rows and replies are variants/vectors —
//    no owning pointers anywhere in this layer.
//  - Every fallible operation returns std::expected<T, Error>; Error
//    distinguishes transport failures, protocol violations and server ERR
//    replies (whose stat code survives for SQLSTATE mapping — BUSY is 100).
//  - Numbers cross as native bytes on read and as std::to_chars
//    shortest-round-trip text on write: exact in both directions and
//    locale-independent (the host application may set the process locale).

#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sqrw {

inline constexpr int protocol_version = 1;
inline constexpr int busy_stat = 100;                    // server ERR stat for cross-connection writes
// Protocol framing limits — fixed by the wire protocol (they mirror
// sqr_net's server-side values), not tunables of this client.
inline constexpr std::size_t max_line = 1024;            // header-line cap (bytes, excluding the LF)
inline constexpr std::size_t max_payload = 1024 * 1024;  // payload cap (1 MiB)
inline constexpr std::size_t max_name = 32;              // table/column name cap (SQR_NAME_LEN)

// Wire column types (the COL line's type token).
enum class ColType { Int, Real, Char, Text };

// One failure.  `stat` is meaningful for Kind::server only (the engine's
// SQR_* code, or busy_stat).
struct [[nodiscard]] Error {
    enum class Kind { transport, protocol, server };
    Kind kind = Kind::transport;
    int stat = 0;
    std::string message = {};

    [[nodiscard]] bool busy() const { return kind == Kind::server && stat == busy_stat; }
};

template <class T>
using Result = std::expected<T, Error>;

// One cell: SQL NULL, an INT, a REAL, or CHAR/TEXT bytes (which of the two
// the column's ColType says).
using Cell = std::variant<std::monostate, std::int32_t, double, std::string>;

// One output column as described by a COL line (SELECT results) or a
// COLUMNS catalogue row.
struct Column {
    std::string name = {};
    ColType type = ColType::Int;
    int csize = 0;            // declared size in bytes
    bool nullable = true;     // always true today: sqr has no NOT NULL
    int key_ordinal = 0;      // 1-based position in the designated unique key, 0 = not part of it
};

// A complete materialised result set.
struct RowSet {
    std::vector<Column> columns = {};
    std::vector<std::vector<Cell>> rows = {};
};

// The four success replies (mirroring sql_result_t):
struct None {};                          // PING / QUIT
struct Count { int n = 0; };             // DML row count
struct Msg { std::string text = {}; };   // DDL and BEGIN/COMMIT/ROLLBACK status
using Reply = std::variant<None, Count, Msg, RowSet>;

// ---- SQL literal helpers (write path) ----
// sql_real formats shortest-round-trip (std::to_chars), which satisfies the
// protocol's 17-significant-digit rule; sql_quote doubles embedded quotes.
[[nodiscard]] std::string sql_real(double v);
[[nodiscard]] std::string sql_int(std::int64_t v);
[[nodiscard]] std::string sql_quote(std::string_view s);

// One connection to a sqrd instance.  Obtain with open(); movable, not
// copyable; the destructor closes the socket (quit() first for an orderly
// goodbye).  Calls are blocking and the connection is single-threaded by
// design — one outstanding request, like the protocol itself.
class Connection {
public:
    Connection() = default;
    ~Connection();
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Connect to host:port (numeric IPv4 text, e.g. "127.0.0.1") and
    // perform the HELLO exchange, announcing `client_name`.
    // `timeout_seconds` bounds the connect and every subsequent send/recv
    // (0 = wait forever).
    [[nodiscard]] static Result<Connection> open(std::string_view host, int port,
                                                 std::string_view client_name,
                                                 int timeout_seconds = 0);

    // Re-arm the I/O timeout on the live socket (0 = none).  A request that
    // times out closes the connection: the stream can never be resynced.
    void set_timeout(int seconds);

    [[nodiscard]] bool is_open() const { return fd_ >= 0; }
    [[nodiscard]] const std::string& server_version() const { return version_; }
    [[nodiscard]] const std::string& dbname() const { return dbname_; }

    // Execute one SQL statement; any of the four reply kinds may come back.
    Result<Reply> exec(std::string_view sql);

    // exec() that must produce a result set (a SELECT).
    Result<RowSet> query(std::string_view sql);

    // Catalogue requests.
    Result<std::vector<std::string>> tables();
    Result<std::vector<Column>> columns(std::string_view table);

    // Liveness check.
    Result<None> ping();

    // Orderly goodbye: QUIT, wait for the acknowledgement, close.  The
    // connection is closed afterwards regardless of the outcome.
    void quit();

private:
    [[nodiscard]] Result<None> send_all(std::string_view bytes);
    [[nodiscard]] Result<None> send_frame(std::string_view line, std::string_view payload);
    [[nodiscard]] Result<None> fill();
    [[nodiscard]] Result<std::string> recv_line();
    [[nodiscard]] Result<std::string> recv_payload(std::size_t n);
    [[nodiscard]] Result<Reply> read_reply();
    [[nodiscard]] Result<RowSet> read_rowset(std::string_view header);
    void close_fd();

    int fd_ = -1;
    std::string rbuf_ = {};   // buffered received bytes
    std::size_t rpos_ = 0;    // first unconsumed position in rbuf_
    std::string version_ = {};
    std::string dbname_ = {};
};

} // namespace sqrw
