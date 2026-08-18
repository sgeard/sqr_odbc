// wire_test.cxx — conformance tests for the sqrw wire client against a real
// sqrd, which the test spawns itself on a scratch database and an ephemeral
// port (reading the port back from sqrd's LISTENING line).
//
// Usage: wire_test <path-to-sqrd>

#include "wire.hxx"
#include "testutil.hxx"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdio>
#include <filesystem>
#include <format>
#include <print>
#include <string>

namespace {

    using testutil::check;

    constexpr const char* dbdir = "wire_test_db";

    sqrw::Connection dial(int port) {
        auto c = sqrw::Connection::open("127.0.0.1", port, "wire_test");
        check(c.has_value(), "connection opens");
        return c ? std::move(*c) : sqrw::Connection{};
    }

    bool is_msg(const sqrw::Result<sqrw::Reply>& r) {
        return r && std::holds_alternative<sqrw::Msg>(*r);
    }

    bool is_count(const sqrw::Result<sqrw::Reply>& r, int n) {
        if (!r) {
            return false;
        }
        const auto* c = std::get_if<sqrw::Count>(&*r);
        return c != nullptr && c->n == n;
    }

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::println(stderr, "usage: wire_test <path-to-sqrd>");
        return 2;
    }
    std::filesystem::remove_all(dbdir);
    const auto srv = testutil::spawn_sqrd(argv[1], dbdir);
    check(srv.port > 0, "sqrd spawned on an ephemeral port");
    if (srv.port <= 0) {
        return 1;
    }

    {
        // ---- handshake ----
        auto bad = sqrw::Connection::open("127.0.0.1", 1, "wire_test");
        check(!bad && bad.error().kind == sqrw::Error::Kind::transport,
              "dead port is a transport error");

        auto c = dial(srv.port);
        check(c.server_version() == "1.0.0", "server version announced");
        check(c.dbname() == dbdir, "database name announced");
        // INFO runs as part of the handshake: the endpoint is a host and a
        // port, so this is the only thing that says where on disk it lands.
        check(c.dbdir() == std::filesystem::absolute(dbdir).lexically_normal().string(),
              "INFO reports the absolute database directory");
        check(c.info("server") == "sqrd 1.0.0", "INFO reports the server version");
        check(c.info("txn") == "no", "INFO reports no transaction in flight");
        check(c.info("nosuchkey").empty(), "INFO returns empty for an unknown key");
        check(c.ping().has_value(), "PING answered");

        // ---- DDL / DML replies ----
        check(is_msg(c.exec("CREATE TABLE t (id INTEGER, x REAL, name CHAR(12), note TEXT)")),
              "CREATE TABLE -> Msg");
        check(is_msg(c.exec("CREATE UNIQUE INDEX ON t (id)")), "CREATE UNIQUE INDEX -> Msg");
        const auto third = 1.0 / 3.0;
        check(is_count(c.exec("INSERT INTO t VALUES (1, 0.1, 'Alice', 'first')"), 1),
              "INSERT -> Count 1");
        check(is_count(c.exec(std::format("INSERT INTO t VALUES (2, {}, {}, 'x')",
                                          sqrw::sql_real(-1.5e-300),
                                          sqrw::sql_quote("O'Brien"))), 1),
              "INSERT with formatted literals");
        check(is_count(c.exec("INSERT INTO t (id) VALUES (3)"), 1), "partial INSERT (NULL row)");
        check(is_count(c.exec(std::format("UPDATE t SET x = {} WHERE id = 3",
                                          sqrw::sql_real(third))), 1),
              "UPDATE with a shortest-round-trip literal");

        // ---- result sets: shape, metadata, binary exactness ----
        auto rs = c.query("SELECT * FROM t ORDER BY id");
        check(rs.has_value() && rs->rows.size() == 3 && rs->columns.size() == 4,
              "SELECT frames 3x4");
        if (rs) {
            const auto& cols = rs->columns;
            check(cols[0].name == "id" && cols[0].type == sqrw::ColType::Int && cols[0].csize == 4,
                  "INT column described");
            check(cols[1].type == sqrw::ColType::Real && cols[1].csize == 8, "REAL column described");
            check(cols[2].type == sqrw::ColType::Char && cols[2].csize == 12, "CHAR column described");
            check(cols[3].type == sqrw::ColType::Text, "TEXT column described");
            const auto& rows = rs->rows;
            check(std::get<std::int32_t>(rows[0][0]) == 1
                      && std::get<std::int32_t>(rows[2][0]) == 3,
                  "INT cells decode");
            check(std::get<double>(rows[0][1]) == 0.1, "REAL cell is bit-exact (0.1)");
            check(std::get<double>(rows[1][1]) == -1.5e-300,
                  "REAL cell survives an extreme exponent");
            check(std::get<double>(rows[2][1]) == third,
                  "written literal round-trips bit-exact (1/3)");
            check(std::get<std::string>(rows[0][2]) == "Alice", "CHAR arrives trimmed");
            check(std::get<std::string>(rows[1][2]) == "O'Brien", "quoted quote round-trips");
            check(std::get<std::string>(rows[0][3]) == "first", "TEXT arrives verbatim");
            check(std::holds_alternative<std::monostate>(rows[2][2])
                      && std::holds_alternative<std::monostate>(rows[2][3]),
                  "NULL cells are NULL");
        }
        auto none = c.query("SELECT * FROM t WHERE id = 99");
        check(none.has_value() && none->rows.empty() && none->columns.size() == 4,
              "empty result keeps its metadata");

        // ---- catalogue ----
        auto tabs = c.tables();
        check(tabs.has_value() && tabs->size() == 1 && (*tabs)[0] == "t", "TABLES lists t");
        auto cols = c.columns("t");
        check(cols.has_value() && cols->size() == 4, "COLUMNS describes 4 columns");
        if (cols) {
            check((*cols)[0].name == "id" && (*cols)[0].key_ordinal == 1,
                  "unique index gives id key-ordinal 1");
            check((*cols)[1].key_ordinal == 0 && (*cols)[3].type == sqrw::ColType::Text,
                  "non-key ordinals and types");
        }
        auto nocols = c.columns("nosuch");
        check(!nocols && nocols.error().kind == sqrw::Error::Kind::server
                  && !nocols.error().message.empty(),
              "COLUMNS on a missing table is a server error");
        // A name that could break line framing is rejected before a byte is
        // sent — and the connection must still be in sync afterwards.
        auto badname = c.columns("no such\ntable");
        check(!badname && badname.error().kind == sqrw::Error::Kind::protocol,
              "framing-hostile table name rejected client-side");
        check(c.ping().has_value(), "connection still in sync after the rejection");

        // ---- errors ----
        auto bad_sql = c.exec("SELEC oops");
        check(!bad_sql && bad_sql.error().kind == sqrw::Error::Kind::server
                  && !bad_sql.error().message.empty(),
              "bad SQL is a server error with a message");
        auto notrows = c.query("INSERT INTO t (id) VALUES (4)");
        check(!notrows && notrows.error().kind == sqrw::Error::Kind::protocol,
              "query() rejects a non-rows reply");
        auto huge = c.exec(std::string(sqrw::max_payload + 1, ' '));
        check(!huge && huge.error().kind == sqrw::Error::Kind::protocol,
              "oversized SQL rejected client-side");

        // ---- BUSY across two connections ----
        auto c2 = dial(srv.port);
        check(is_msg(c.exec("BEGIN")), "BEGIN -> Msg");
        auto busy = c2.exec("DELETE FROM t WHERE id = 4");
        check(!busy && busy.error().busy(), "cross-connection write is BUSY (stat 100)");
        auto readok = c2.query("SELECT id FROM t WHERE id = 1");
        check(readok.has_value() && readok->rows.size() == 1,
              "cross-connection read still allowed");
        check(is_msg(c.exec("ROLLBACK")), "ROLLBACK -> Msg");
        check(is_count(c2.exec("DELETE FROM t WHERE id = 4"), 1),
              "write allowed once the transaction closes");
        c2.quit();
        check(!c2.is_open(), "quit closes the connection");

        // ---- timeout against a silent listener ----
        // A socket that listens but never speaks: connect succeeds (kernel
        // backlog), the HELLO reply never comes, and the 1 s I/O timeout
        // must turn that into a transport error rather than a hang.
        {
            const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a = {};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = 0;
            ::bind(lfd, reinterpret_cast<const sockaddr*>(&a), sizeof a);
            ::listen(lfd, 1);
            socklen_t alen = sizeof a;
            ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &alen);
            auto mute = sqrw::Connection::open("127.0.0.1", ntohs(a.sin_port), "wire_test", 1);
            check(!mute && mute.error().kind == sqrw::Error::Kind::transport
                      && mute.error().message.find("timed out") != std::string::npos,
                  "silent server times out with a transport error");
            ::close(lfd);
        }

        // ---- server disappears ----
        testutil::stop_sqrd(srv);
        auto gone = c.exec("SELECT * FROM t");
        check(!gone && gone.error().kind == sqrw::Error::Kind::transport,
              "killed server is a transport error");
    }

    std::filesystem::remove_all(dbdir);
    return testutil::summary("wire tests");
}
