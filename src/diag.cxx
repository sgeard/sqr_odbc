// diag.cxx — SQLSTATE mapping for wire-layer failures (see diag.hxx).

#include "diag.hxx"

namespace sqro {

namespace {

    // The engine's SQR_* stat codes (sqr.f90) — the wire carries them
    // verbatim in ERR replies, plus the server's own BUSY.
    enum : int {
        sqr_not_found = 1,
        sqr_dup = 2,
        sqr_invalid = 5,
        sqr_readonly = 6,
    };

} // namespace

std::string_view sqlstate_for(const sqrw::Error& e) {
    switch (e.kind) {
    case sqrw::Error::Kind::transport:
        return "08S01";                    // communication link failure
    case sqrw::Error::Kind::protocol:
        return "HY000";                    // driver-internal: general error
    case sqrw::Error::Kind::server:
        break;
    }
    if (e.stat == sqrw::busy_stat) {
        return "40001";                    // serialization failure (BUSY)
    }
    switch (e.stat) {
    case sqr_not_found: return "42S02";    // base table not found
    case sqr_dup:       return "23000";    // integrity constraint violation
    case sqr_invalid:   return "42000";    // syntax error or access violation
    case sqr_readonly:  return "25006";    // read-only transaction
    default:            return "HY000";
    }
}

} // namespace sqro
