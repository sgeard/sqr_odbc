# sqr_odbc — ODBC driver for the sqr database server

A C++23 ODBC driver (ODBC 3.8 Core, ANSI) connecting ODBC applications —
first target LibreOffice — to a sqr database served by `sqrd`. The server
comes from the sqr project (github.com/sgeard/sqr), expected as a sibling
checkout at `../sqr`.

## Build and test

```
make                # CXX=icpx (default) or CXX=g++; debug=1 opt-in
make utest          # unit tests: wire (42) + model (72) + entry points (81)
make isqltest       # functional test: a full session through unixODBC's isql
```

The test programs spawn a real `sqrd` (default
`../sqr/obj_intel_release/sqrd`, override with `SQRD=`) on a scratch
database and an ephemeral port.

## Install and register

```
make install        # copies libsqrodbc.so to ~/.local/lib/sqrodbc/
make install-dsn    # registers the [sqr] data source in ~/.odbc.ini
```

Neither needs root. The data source points at the installed library by
full path, so no `odbcinst.ini` entry is required; edit `~/.odbc.ini` to
change `SERVER`/`PORT` (default 127.0.0.1:7477). The database is
whichever one the sqrd instance you connect to is serving.

Connection-string keys (for `SQLDriverConnect` users such as `isql -k`):
`DSN`, `SERVER`, `PORT`, `TIMEOUT`, `DATABASE`, and `DRIVER` as either a
registered name or the .so path. Explicit keys beat the DSN entry, which
beats the defaults.

### `DATABASE` — name the database, not the port

The best DSN names the database directory and leaves `PORT` out:

```
[sqr]
Driver   = /home/simon/.local/lib/sqrodbc/libsqrodbc.so
DATABASE = /home/simon/db/accounts
```

`sqrd` records the port it bound in `<db-dir>/_sqrd`, and the driver reads it
from there. A port number identifies nothing a person recognises and goes
stale the moment a daemon restarts elsewhere; a directory is the thing you
actually mean, and it survives.

`DATABASE` is never a *selector* — sqrd serves the one database it was
started with and the protocol cannot ask for another. It is also an
**assertion**: the driver connects, asks the server which directory it is
serving, and fails with SQLSTATE `08001` if the answer disagrees. That check
is what makes the `_sqrd` file safe to use, since nothing removes it when a
daemon dies, and it equally catches a recycled port or a reused pid.

Matching is permissive: the short name, the directory as the server was
given it, its fully resolved form, or any of those normalised. **Symbolic
links work** — `DATABASE=/home/simon/db/current` matches a daemon serving
that link, and so does the path it resolves to.

`PORT` still works and still wins when given, so existing DSNs are
unaffected; `sqrbak info <db-dir>` (in the sqr project) prints everything a
DSN might want.

The driver also reports the absolute directory through
`SQLGetInfo(SQL_DATABASE_NAME)`, as the SQLite driver reports its file
path, so an application asking which database it is on gets somewhere on
disk rather than a bare name.

## Quick check

With a server running (`sqrd <db-dir> 7477`):

```
isql -v sqr
SQL> SELECT * FROM some_table;
```

## LibreOffice Base

1. Start sqrd on the database: `sqrd <db-dir> 7477`.
2. Base: *File → New → Database → Connect to an existing database → ODBC*,
   pick `sqr`, no user name or password, finish and save the .odb.
3. Tables appear under *Tables*; rows can be browsed and — for tables with
   a unique index — edited. *Tools → SQL* passes statements straight
   through for anything in sqr's SQL subset.

Calc can then pull the data via *Insert → Sheet from file* alternatives or
the registered data source (View → Data Sources, F4), or keep using the
sqr Calc client in `../sqr/calc` for the push/pull workflow.

## Layers

Three layers: wire client (`sqrw`), object model (`sqro`), extern "C"
trampolines packaged as a self-contained `libsqrodbc.so` (only
libodbcinst/libc/libm dynamic). The wire protocol is defined in the sqr
project's `reports/DESIGN-wire-protocol.md`.
