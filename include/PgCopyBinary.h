#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Raw PostgreSQL binary COPY protocol reader/writer -- bypasses pqxx
// entirely for this, since pqxx (7.10.0, as linked in this project) has no
// raw binary COPY primitive in either direction (stream_from/
// transaction_base::stream are hardwired to COPY TEXT format with typed or
// text-string decoding; pqxx::connection has no public accessor to a live
// PGconn* either -- confirmed by header inspection this session). Uses
// plain libpq directly instead (PQconnectdb/PQexec/PQgetCopyData) --
// libpq-fe.h and libpq are already linked via this project's existing
// PQ_INCLUDE/PQ_LIB CMake variables, no new dependency. libpq-fe.h itself
// is only included in the .cpp, not here, so callers of this header don't
// need it transitively.
namespace PgCopyBinary {

// One field within a row, as read directly off the wire: a length prefix
// (-1 = NULL) and, if not NULL, a pointer to that many raw bytes *within
// the owning Row's `raw` buffer* -- valid only for that Row's lifetime.
struct Field {
    const uint8_t* data;  // nullptr if NULL
    int32_t len;          // -1 if NULL, else byte length
};

// A single COPY row. `fields` are views into `raw` (see Field above), so
// Row is move-only -- copying would need deep repointing, and nothing in
// this codebase's usage needs to copy a Row (each is consumed immediately
// by the caller's loop, never accumulated into a container that could
// reallocate and silently invalidate the field pointers).
struct Row {
    Row() = default;
    Row(Row&&) = default;
    Row& operator=(Row&&) = default;
    Row(const Row&) = delete;
    Row& operator=(const Row&) = delete;

    std::vector<uint8_t> raw;
    std::vector<Field> fields;
};

// Opens a dedicated raw libpq connection and starts `COPY (<select_sql>)
// TO STDOUT WITH (FORMAT binary)`. Consumes and validates the 19-byte
// COPY-binary file header as part of construction. Throws
// std::runtime_error on any connection/query/header failure.
class Reader {
public:
    Reader(const std::string& conninfo, const std::string& select_sql);
    ~Reader();

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // Reads and parses the next row. Returns std::nullopt at the trailer
    // (int16(-1)) / end of stream (after which the underlying command is
    // fully drained via PQgetResult). Throws std::runtime_error on a
    // malformed row (declared field count/lengths don't fit the row's
    // actual byte length) or a mid-stream libpq error.
    std::optional<Row> next();

private:
    void* conn_ = nullptr;  // PGconn*

    // PQgetCopyData's chunk boundaries are wire-message boundaries, not row
    // or header boundaries -- a single chunk can bundle the 19-byte header
    // together with several rows (small result sets), and a row can be
    // split across chunks (large results). buf_/pos_ is a simple streaming
    // byte buffer that decouples parsing from chunking: fill() pulls more
    // wire chunks only when the buffered-but-unconsumed bytes (from pos_
    // onward) are fewer than what's needed.
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
    bool eof_ = false;

    // Earliest offset into buf_ that next() still needs (the start of the
    // row currently being parsed, or pos_ when not mid-row). fill()
    // compacts buf_ by erasing everything before mark_, not before pos_,
    // and keeps mark_ in sync with the erase so that offsets captured
    // relative to mark_ mid-row (see next()) stay valid across a
    // compaction that happens partway through parsing one row.
    size_t mark_ = 0;

    // Ensures at least `need` unconsumed bytes are available from pos_
    // onward, pulling and appending more wire chunks as needed. Throws if
    // the COPY stream ends (PQgetCopyData returns -1) before `need` bytes
    // arrive -- that's always malformed data, since the real end of a
    // well-formed stream is only ever signaled right after the trailer
    // bytes have already been delivered and consumed.
    void fill(size_t need);

    // Drains any remaining wire chunks after the trailer has been consumed
    // and calls PQgetResult to fully complete the command server-side.
    void finish();
};

// bigint fields (e.g. an `id` column) are sent in PostgreSQL binary COPY's
// network byte order (big-endian) regardless of this codebase's WKB
// payloads being little-endian (PostGIS's geometry_send passes the
// internally-stored EWKB bytes through verbatim, unlike int8send which
// explicitly byte-swaps) -- use this, not a raw memcpy, to read one.
// Throws std::runtime_error if the field isn't a non-NULL 8-byte value.
int64_t readBigintBE(const Field& f);

// Hand-rolled binary-COPY container writer -- the output format
// regional_install.cpp's existing `\copy <table> FROM '<file>' WITH
// (FORMAT binary)` already expects, so nothing changes on the install
// side. Call writeRow() with the exact unmodified bytes of a row this
// process decided to keep (true passthrough -- nothing is reconstructed,
// since nothing in a kept row's payload is ever transformed) for every
// matching row, then close() (or let the destructor do it) to emit the
// trailer.
class Writer {
public:
    explicit Writer(const std::string& path);
    ~Writer();

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // `raw` is one row's complete bytes exactly as returned by Reader
    // (Row::raw) -- written verbatim, no reparsing.
    void writeRow(const std::vector<uint8_t>& raw);

    void close();

private:
    void* f_ = nullptr;  // FILE*
    bool closed_ = false;
};

} // namespace PgCopyBinary
