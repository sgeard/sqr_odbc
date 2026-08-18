// diag.hxx — ODBC diagnostics for the sqr driver's object model.
//
// Every handle (Environment / Connection / Statement) carries a DiagStack:
// cleared at the start of each operation on that handle, appended to on
// failure or warning, read back afterwards through SQLGetDiagRec.  This is
// also where wire-layer failures are translated into SQLSTATEs — notably
// the server's BUSY stat 100 -> 40001 (serialization failure).

#pragma once

#include "wire.hxx"

#include <string>
#include <string_view>
#include <vector>

namespace sqro {

// The SQLSTATE for a wire-layer error.
[[nodiscard]] std::string_view sqlstate_for(const sqrw::Error& e);

struct DiagRecord {
    std::string sqlstate = "HY000";  // five characters
    int native = 0;                  // wire stat code (server errors), else 0
    std::string message = {};
};

class DiagStack {
public:
    void clear() { recs_.clear(); }

    void post(std::string_view sqlstate, std::string message, int native = 0) {
        recs_.push_back({.sqlstate = std::string(sqlstate),
                         .native = native,
                         .message = std::move(message)});
    }

    // Translate and post a wire-layer failure.
    void post(const sqrw::Error& e) {
        post(sqlstate_for(e), e.message, e.stat);
    }

    [[nodiscard]] bool empty() const { return recs_.empty(); }
    [[nodiscard]] std::size_t size() const { return recs_.size(); }

    // 1-based, as SQLGetDiagRec counts; nullptr when out of range.
    [[nodiscard]] const DiagRecord* record(std::size_t recnum) const {
        return recnum >= 1 && recnum <= recs_.size() ? &recs_[recnum - 1] : nullptr;
    }

    // SQL_DIAG_RETURNCODE: the SQLRETURN of the last operation on this
    // handle, recorded by the trampolines (short = SQLSMALLINT = SQLRETURN).
    void set_return_code(short rc) { return_code_ = rc; }
    [[nodiscard]] short return_code() const { return return_code_; }

private:
    std::vector<DiagRecord> recs_ = {};
    short return_code_ = 0;
};

} // namespace sqro
