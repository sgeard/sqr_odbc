# Functional test for the ODBC driver: a complete session through unixODBC's
# isql, the same load path LibreOffice uses (driver manager -> SQLDriverConnect
# -> libsqrodbc.so -> sqrd).  DRIVER= carries the library path directly, so
# the test needs no DSN and touches no configuration files.
#
# A real sqrd is spawned on an ephemeral port; the port is read back from the
# LISTENING line on its stdout.
#
# Usage: tclsh run_isql.tcl <path-to-sqrd> <path-to-libsqrodbc.so>

if {$::argc != 2} {
    puts stderr "usage: tclsh run_isql.tcl <path-to-sqrd> <path-to-libsqrodbc.so>"
    exit 2
}
lassign $::argv exe so
set so [file normalize $so]
set dbdir isql_func_db
file delete -force $dbdir

set npass 0
set nfail 0
# cond arrives braced and is evaluated in the caller's scope.
proc check {cond label} {
    if {[uplevel 1 [list expr $cond]]} {
        incr ::npass
        puts "  OK   $label"
    } else {
        incr ::nfail
        puts "  FAIL $label"
    }
}

# ---- start the server, read the port back ----
set srv [open [list | $exe $dbdir 0] r]
fconfigure $srv -blocking 1
set line [gets $srv]
check {[lindex $line 0] eq "LISTENING"} "server announces LISTENING"
set port [lindex $line 1]

# ---- one isql batch session ----
set session [join {
    {CREATE TABLE tt (id INTEGER, x REAL, name CHAR(8))}
    {CREATE UNIQUE INDEX ON tt (id)}
    {INSERT INTO tt VALUES (1, 0.1, 'alpha')}
    {INSERT INTO tt VALUES (2, -1.5e-300, NULL)}
    {SELECT * FROM tt ORDER BY id}
    {DELETE FROM tt WHERE id = 2}
    {SELECT id FROM tt}
    {CREATE TABLE qt ("Account#" INTEGER, "Rate %" REAL)}
    {INSERT INTO qt ("Account#", "Rate %") VALUES (7, 4.5)}
    {SELECT "Account#" FROM qt WHERE "Rate %" > 4}
    {SELECT "Rate %" FROM qt}
    {SELECT COUNT(*) AS total, MAX(id) FROM tt}
    {SELECT 42, 'lit' AS word}
    {SELECT id FROM tt WHERE name LIKE 'al%' AND id IN (1, 2)}
    {SELEC oops}
    {help}
    {help tt}
} \n]

set conn "DRIVER=$so;SERVER=127.0.0.1;PORT=$port"
# -v: without it isql suppresses the driver's diagnostic records.  isql
# prints SQL errors on stderr and keeps going; fold the streams together and
# never let a non-zero exit hide the output.
catch {exec isql -v -k $conn -b << $session 2>@1} out

# ---- checks against the transcript ----
check {[regexp -all {SQLRowCount returns 1} $out] >= 3} \
    "inserts and delete each report one row"
check {[string match *alpha* $out]} "CHAR value survives the round trip"
check {[string match *0.1* $out]} "REAL prints shortest-round-trip"
check {[string match *-1.5e-300* $out]} "tiny REAL survives bit-exact"
# isql declares ODBC 2.x, so the driver manager maps our 42000 (syntax
# error, 3.x) to its 2.x equivalent 37000 before isql prints it.
check {[string match *37000* $out]} "SQL error carries its SQLSTATE (42000 -> 2.x 37000)"
check {[string match "*2 rows fetched*" $out]} "first SELECT fetches both rows"
check {[regexp {\|\s*Rate %} $out]} "column name with a space heads its result"
check {[regexp {\|\s*total\s*\|\s*MAX\(id\)} $out]} "aggregates arrive under alias and canonical name"
check {[regexp {\|\s*42\s*\|\s*lit\s*\|} $out]} "SELECT of literals needs no table"
check {![string match "*bad COL line*" $out]} "no COL framing error anywhere in the session"
check {[string match "*1 rows fetched*" $out]} "SELECT after DELETE fetches one row"
check {[string match "*Account#*" $out] && [string match *7* $out]} \
    "quoted identifiers (Account#, Rate %) work end-to-end"
check {[string match *TABLE_NAME* $out] && [string match *tt* $out]} \
    "help (SQLTables) lists the table"
check {[string match *COLUMN_NAME* $out] && [string match *name* $out]} \
    "help tt (SQLColumns) lists the columns"

# ---- shut down ----
exec kill -9 [pid $srv]
catch {close $srv}
file delete -force $dbdir
puts "isql tests: $npass passed, $nfail failed"
exit [expr {$nfail > 0 ? 1 : 0}]
