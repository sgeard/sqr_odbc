// wire.cxx — implementation of the sqrd wire-protocol client (see wire.hxx).

#include "wire.hxx"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <format>
#include <utility>

// Binary cells are little-endian on the wire ("a straight transfer on
// x86-64/ARM"); this client assumes a little-endian host.
static_assert(std::endian::native == std::endian::little,
              "sqrw::Connection requires a little-endian host");

namespace sqrw {

namespace {

    constexpr std::size_t recv_chunk = 65536;

    [[nodiscard]] Error transport(std::string message) {
        return {.kind = Error::Kind::transport, .stat = 0, .message = std::move(message)};
    }

    [[nodiscard]] Error transport_errno(std::string_view what) {
        return transport(std::format("{}: {}", what, std::strerror(errno)));
    }

    [[nodiscard]] Error protocol(std::string message) {
        return {.kind = Error::Kind::protocol, .stat = 0, .message = std::move(message)};
    }

    // Split a header line into space-separated tokens (views into `line`).
    [[nodiscard]] std::vector<std::string_view> split(std::string_view line) {
        std::vector<std::string_view> toks;
        std::size_t i = 0;
        while (i < line.size()) {
            const auto sp = line.find(' ', i);
            const auto end = sp == std::string_view::npos ? line.size() : sp;
            if (end > i) {
                toks.push_back(line.substr(i, end - i));
            }
            i = end + 1;
        }
        return toks;
    }

    // Strict non-negative decimal integer, or nullopt.
    [[nodiscard]] std::expected<std::size_t, Error> parse_count(std::string_view tok) {
        std::size_t v = 0;
        const auto [p, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), v);
        if (ec != std::errc{} || p != tok.data() + tok.size()) {
            return std::unexpected(protocol(std::format("bad count \"{}\"", tok)));
        }
        return v;
    }

    [[nodiscard]] std::expected<ColType, Error> parse_coltype(std::string_view tok) {
        if (tok == "INT") { return ColType::Int; }
        if (tok == "REAL") { return ColType::Real; }
        if (tok == "CHAR") { return ColType::Char; }
        if (tok == "TEXT") { return ColType::Text; }
        return std::unexpected(protocol(std::format("unknown column type \"{}\"", tok)));
    }

    // Decode one non-NULL cell payload according to its column type.
    [[nodiscard]] std::expected<Cell, Error> decode_cell(ColType type, std::string&& bytes) {
        switch (type) {
        case ColType::Int: {
            if (bytes.size() != 4) {
                return std::unexpected(protocol("INT cell is not 4 bytes"));
            }
            std::array<char, 4> raw = {};
            std::memcpy(raw.data(), bytes.data(), raw.size());
            return Cell{std::bit_cast<std::int32_t>(raw)};
        }
        case ColType::Real: {
            if (bytes.size() != 8) {
                return std::unexpected(protocol("REAL cell is not 8 bytes"));
            }
            std::array<char, 8> raw = {};
            std::memcpy(raw.data(), bytes.data(), raw.size());
            return Cell{std::bit_cast<double>(raw)};
        }
        case ColType::Char:
        case ColType::Text:
            return Cell{std::move(bytes)};
        }
        return std::unexpected(protocol("bad column type"));
    }

} // namespace

// ---- SQL literal helpers ----

std::string sql_real(double v) {
    std::array<char, 32> buf = {};
    const auto [p, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    return ec == std::errc{} ? std::string(buf.data(), p) : std::string("0");
}

std::string sql_int(std::int32_t v) {
    std::array<char, 16> buf = {};
    const auto [p, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    return ec == std::errc{} ? std::string(buf.data(), p) : std::string("0");
}

std::string sql_quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (const auto c : s) {
        if (c == '\'') {
            out.push_back('\'');
        }
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

// ---- connection lifecycle ----

Connection::~Connection() {
    close_fd();
}

Connection::Connection(Connection&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      rbuf_(std::move(other.rbuf_)),
      rpos_(std::exchange(other.rpos_, 0)),
      version_(std::move(other.version_)),
      dbname_(std::move(other.dbname_)) {
}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        close_fd();
        fd_ = std::exchange(other.fd_, -1);
        rbuf_ = std::move(other.rbuf_);
        rpos_ = std::exchange(other.rpos_, 0);
        version_ = std::move(other.version_);
        dbname_ = std::move(other.dbname_);
    }
    return *this;
}

void Connection::close_fd() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

Result<Connection> Connection::open(std::string_view host, int port,
                                    std::string_view client_name) {
    Connection c;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, std::string(host).c_str(), &addr.sin_addr) != 1) {
        return std::unexpected(transport(std::format("bad host \"{}\" (numeric IPv4 expected)", host)));
    }
    c.fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (c.fd_ < 0) {
        return std::unexpected(transport_errno("socket"));
    }
    int rc = 0;
    do {
        rc = ::connect(c.fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
    } while (rc < 0 && errno == EINTR);
    if (rc != 0) {
        return std::unexpected(transport_errno(std::format("connect to {}:{}", host, port)));
    }

    if (auto sent = c.send_all(std::format("HELLO {} {}\n", protocol_version, client_name)); !sent) {
        return std::unexpected(sent.error());
    }
    auto line = c.recv_line();
    if (!line) {
        return std::unexpected(line.error());
    }
    const auto toks = split(*line);
    if (toks.size() >= 3 && toks[0] == "ERR") {
        auto n = parse_count(toks[2]);
        if (!n) {
            return std::unexpected(n.error());
        }
        auto text = c.recv_payload(*n);
        return std::unexpected(Error{.kind = Error::Kind::server,
                                     .stat = 0,
                                     .message = text ? std::move(*text) : "handshake refused"});
    }
    if (toks.size() < 5 || toks[0] != "OK" || toks[1] != "1" || toks[2] != "sqrd") {
        return std::unexpected(protocol(std::format("unexpected handshake reply \"{}\"", *line)));
    }
    c.version_ = std::string(toks[3]);
    c.dbname_ = std::string(toks[4]);
    return c;
}

void Connection::quit() {
    if (fd_ < 0) {
        return;
    }
    if (send_all("QUIT\n")) {
        (void)recv_line();      // best-effort NONE acknowledgement
    }
    close_fd();
}

// ---- framing ----

Result<None> Connection::send_all(std::string_view bytes) {
    if (fd_ < 0) {
        return std::unexpected(transport("connection is closed"));
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto n = ::send(fd_, bytes.data() + done, bytes.size() - done, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(transport_errno("send"));
        }
        done += static_cast<std::size_t>(n);
    }
    return None{};
}

Result<None> Connection::send_frame(std::string_view line, std::string_view payload) {
    std::string frame;
    frame.reserve(line.size() + 1 + payload.size());
    frame.append(line);
    frame.push_back('\n');
    frame.append(payload);
    return send_all(frame);
}

Result<None> Connection::fill() {
    if (rpos_ > 0) {
        rbuf_.erase(0, rpos_);
        rpos_ = 0;
    }
    std::array<char, recv_chunk> chunk = {};
    ssize_t n = 0;
    do {
        n = ::recv(fd_, chunk.data(), chunk.size(), 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        return std::unexpected(transport_errno("recv"));
    }
    if (n == 0) {
        return std::unexpected(transport("server closed the connection"));
    }
    rbuf_.append(chunk.data(), static_cast<std::size_t>(n));
    return None{};
}

Result<std::string> Connection::recv_line() {
    if (fd_ < 0) {
        return std::unexpected(transport("connection is closed"));
    }
    for (;;) {
        const auto nl = rbuf_.find('\n', rpos_);
        if (nl != std::string::npos) {
            if (nl - rpos_ > max_line) {
                return std::unexpected(protocol("header line over limit"));
            }
            std::string line = rbuf_.substr(rpos_, nl - rpos_);
            rpos_ = nl + 1;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }
        if (rbuf_.size() - rpos_ > max_line) {
            return std::unexpected(protocol("header line over limit"));
        }
        if (auto filled = fill(); !filled) {
            return std::unexpected(filled.error());
        }
    }
}

Result<std::string> Connection::recv_payload(std::size_t n) {
    if (n > max_payload) {
        return std::unexpected(protocol("payload over limit"));
    }
    std::string bytes;
    bytes.reserve(n);
    const auto have = std::min(n, rbuf_.size() - rpos_);
    bytes.append(rbuf_, rpos_, have);
    rpos_ += have;
    while (bytes.size() < n) {
        if (auto filled = fill(); !filled) {
            return std::unexpected(filled.error());
        }
        const auto more = std::min(n - bytes.size(), rbuf_.size() - rpos_);
        bytes.append(rbuf_, rpos_, more);
        rpos_ += more;
    }
    return bytes;
}

// ---- replies ----

Result<Reply> Connection::read_reply() {
    auto line = recv_line();
    if (!line) {
        return std::unexpected(line.error());
    }
    const auto toks = split(*line);
    if (toks.empty()) {
        return std::unexpected(protocol("empty reply header"));
    }
    if (toks[0] == "NONE") {
        return Reply{None{}};
    }
    if (toks[0] == "COUNT" && toks.size() == 2) {
        auto n = parse_count(toks[1]);
        if (!n) {
            return std::unexpected(n.error());
        }
        return Reply{Count{.n = static_cast<int>(*n)}};
    }
    if (toks[0] == "MSG" && toks.size() == 2) {
        auto n = parse_count(toks[1]);
        if (!n) {
            return std::unexpected(n.error());
        }
        auto text = recv_payload(*n);
        if (!text) {
            return std::unexpected(text.error());
        }
        return Reply{Msg{.text = std::move(*text)}};
    }
    if (toks[0] == "ERR" && toks.size() == 3) {
        int stat = 0;
        const auto [p, ec] = std::from_chars(toks[1].data(), toks[1].data() + toks[1].size(), stat);
        if (ec != std::errc{} || p != toks[1].data() + toks[1].size()) {
            return std::unexpected(protocol("bad ERR stat"));
        }
        auto n = parse_count(toks[2]);
        if (!n) {
            return std::unexpected(n.error());
        }
        auto text = recv_payload(*n);
        if (!text) {
            return std::unexpected(text.error());
        }
        return std::unexpected(Error{.kind = Error::Kind::server,
                                     .stat = stat,
                                     .message = std::move(*text)});
    }
    if (toks[0] == "ROWS" && toks.size() == 3) {
        return read_rowset(*line).transform([](RowSet&& rs) { return Reply{std::move(rs)}; });
    }
    return std::unexpected(protocol(std::format("unexpected reply \"{}\"", *line)));
}

Result<RowSet> Connection::read_rowset(std::string_view header) {
    const auto htoks = split(header);
    auto nr = parse_count(htoks[1]);
    auto nc = parse_count(htoks[2]);
    if (!nr || !nc) {
        return std::unexpected(protocol("bad ROWS header"));
    }
    RowSet rs;
    rs.columns.reserve(*nc);
    for (std::size_t j = 0; j < *nc; ++j) {
        auto line = recv_line();
        if (!line) {
            return std::unexpected(line.error());
        }
        const auto toks = split(*line);
        // COL name type csize null|notnull key-ordinal
        if (toks.size() != 6 || toks[0] != "COL") {
            return std::unexpected(protocol(std::format("bad COL line \"{}\"", *line)));
        }
        auto type = parse_coltype(toks[2]);
        auto csize = parse_count(toks[3]);
        auto key = parse_count(toks[5]);
        if (!type || !csize || !key) {
            return std::unexpected(protocol(std::format("bad COL line \"{}\"", *line)));
        }
        rs.columns.push_back({.name = std::string(toks[1]),
                              .type = *type,
                              .csize = static_cast<int>(*csize),
                              .nullable = toks[4] == "null",
                              .key_ordinal = static_cast<int>(*key)});
    }
    rs.rows.reserve(*nr);
    for (std::size_t r = 0; r < *nr; ++r) {
        std::vector<Cell> row;
        row.reserve(*nc);
        for (std::size_t j = 0; j < *nc; ++j) {
            auto line = recv_line();
            if (!line) {
                return std::unexpected(line.error());
            }
            if (*line == "N") {
                row.emplace_back(std::monostate{});
                continue;
            }
            const auto toks = split(*line);
            if (toks.size() != 2 || toks[0] != "C") {
                return std::unexpected(protocol(std::format("bad cell line \"{}\"", *line)));
            }
            auto n = parse_count(toks[1]);
            if (!n) {
                return std::unexpected(n.error());
            }
            auto bytes = recv_payload(*n);
            if (!bytes) {
                return std::unexpected(bytes.error());
            }
            auto cell = decode_cell(rs.columns[j].type, std::move(*bytes));
            if (!cell) {
                return std::unexpected(cell.error());
            }
            row.push_back(std::move(*cell));
        }
        rs.rows.push_back(std::move(row));
    }
    auto end = recv_line();
    if (!end) {
        return std::unexpected(end.error());
    }
    if (*end != "END") {
        return std::unexpected(protocol("missing END"));
    }
    return rs;
}

// ---- requests ----

Result<Reply> Connection::exec(std::string_view sql) {
    if (sql.size() > max_payload) {
        return std::unexpected(protocol("SQL text over the 1 MiB payload limit"));
    }
    if (auto sent = send_frame(std::format("SQL {}", sql.size()), sql); !sent) {
        return std::unexpected(sent.error());
    }
    return read_reply();
}

Result<RowSet> Connection::query(std::string_view sql) {
    auto reply = exec(sql);
    if (!reply) {
        return std::unexpected(reply.error());
    }
    if (auto* rs = std::get_if<RowSet>(&*reply)) {
        return std::move(*rs);
    }
    return std::unexpected(protocol("statement did not return rows"));
}

Result<std::vector<std::string>> Connection::tables() {
    if (auto sent = send_all("TABLES\n"); !sent) {
        return std::unexpected(sent.error());
    }
    auto reply = read_reply();
    if (!reply) {
        return std::unexpected(reply.error());
    }
    auto* rs = std::get_if<RowSet>(&*reply);
    if (rs == nullptr || rs->columns.size() != 1) {
        return std::unexpected(protocol("bad TABLES reply"));
    }
    std::vector<std::string> names;
    names.reserve(rs->rows.size());
    for (auto& row : rs->rows) {
        if (auto* s = std::get_if<std::string>(&row[0])) {
            names.push_back(std::move(*s));
        }
    }
    return names;
}

Result<std::vector<Column>> Connection::columns(std::string_view table) {
    if (auto sent = send_all(std::format("COLUMNS {}\n", table)); !sent) {
        return std::unexpected(sent.error());
    }
    auto reply = read_reply();
    if (!reply) {
        return std::unexpected(reply.error());
    }
    auto* rs = std::get_if<RowSet>(&*reply);
    if (rs == nullptr || rs->columns.size() != 5) {
        return std::unexpected(protocol("bad COLUMNS reply"));
    }
    std::vector<Column> cols;
    cols.reserve(rs->rows.size());
    for (auto& row : rs->rows) {
        auto* name = std::get_if<std::string>(&row[0]);
        auto* type = std::get_if<std::string>(&row[1]);
        auto* csize = std::get_if<std::int32_t>(&row[2]);
        auto* nullable = std::get_if<std::string>(&row[3]);
        auto* key = std::get_if<std::int32_t>(&row[4]);
        if (name == nullptr || type == nullptr || csize == nullptr
            || nullable == nullptr || key == nullptr) {
            return std::unexpected(protocol("bad COLUMNS row"));
        }
        auto ct = parse_coltype(*type);
        if (!ct) {
            return std::unexpected(ct.error());
        }
        cols.push_back({.name = std::move(*name),
                        .type = *ct,
                        .csize = *csize,
                        .nullable = *nullable == "null",
                        .key_ordinal = *key});
    }
    return cols;
}

Result<None> Connection::ping() {
    if (auto sent = send_all("PING\n"); !sent) {
        return std::unexpected(sent.error());
    }
    auto reply = read_reply();
    if (!reply) {
        return std::unexpected(reply.error());
    }
    if (!std::holds_alternative<None>(*reply)) {
        return std::unexpected(protocol("unexpected PING reply"));
    }
    return None{};
}

} // namespace sqrw
