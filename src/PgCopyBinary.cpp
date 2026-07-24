#include "PgCopyBinary.h"

#include <libpq-fe.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace PgCopyBinary {

namespace {

// 11-byte fixed signature + 4-byte flags + 4-byte header-extension length
// = 19-byte COPY-binary file header (both flags and extension length are
// always 0 for what we read/write here).
constexpr char kSignature[11] = {'P', 'G', 'C', 'O', 'P', 'Y', '\n', '\xff', '\r', '\n', '\0'};

} // namespace

Reader::Reader(const std::string& conninfo, const std::string& select_sql) {
    PGconn* conn = PQconnectdb(conninfo.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string err = PQerrorMessage(conn);
        PQfinish(conn);
        throw std::runtime_error("PgCopyBinary::Reader: connection failed: " + err);
    }

    std::string sql = "COPY (" + select_sql + ") TO STDOUT WITH (FORMAT binary)";
    PGresult* res = PQexec(conn, sql.c_str());
    if (PQresultStatus(res) != PGRES_COPY_OUT) {
        std::string err = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        throw std::runtime_error("PgCopyBinary::Reader: COPY TO STDOUT failed: " + err);
    }
    PQclear(res);

    conn_ = conn;

    // The 19-byte file header (11-byte signature + 4-byte flags + 4-byte
    // header-extension length) is the start of the raw byte stream, but
    // NOT necessarily its own PQgetCopyData chunk -- a chunk can bundle it
    // with row data (common for small result sets). Pull via fill()
    // rather than assuming chunk alignment.
    try {
        fill(19);
    } catch (const std::exception&) {
        PQfinish(conn);
        conn_ = nullptr;
        throw;
    }
    if (std::memcmp(buf_.data() + pos_, kSignature, sizeof(kSignature)) != 0) {
        PQfinish(conn);
        conn_ = nullptr;
        throw std::runtime_error("PgCopyBinary::Reader: unexpected/missing COPY binary header");
    }
    pos_ += 19;
}

Reader::~Reader() {
    // If the COPY wasn't fully drained (an exception thrown mid-stream, or
    // the caller simply stopped early), PQfinish still cleanly terminates
    // the connection and aborts the COPY server-side -- no separate
    // cleanup needed.
    if (conn_) PQfinish(static_cast<PGconn*>(conn_));
}

void Reader::fill(size_t need) {
    PGconn* conn = static_cast<PGconn*>(conn_);
    while (buf_.size() - pos_ < need) {
        char* chunk = nullptr;
        int len = PQgetCopyData(conn, &chunk, 0);
        if (len == -1) {
            throw std::runtime_error(
                "PgCopyBinary::Reader: COPY stream ended mid-row (malformed/truncated data)");
        }
        if (len == -2) {
            throw std::runtime_error(std::string("PgCopyBinary::Reader: PQgetCopyData error: ") +
                                      PQerrorMessage(conn));
        }
        // Drop bytes before mark_ (never before pos_ -- mark_ tracks the
        // start of the row currently being parsed, see the header comment)
        // so the buffer doesn't grow unboundedly over a long-lived Reader.
        if (mark_ > 0) {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(mark_));
            pos_ -= mark_;
            mark_ = 0;
        }
        buf_.insert(buf_.end(), reinterpret_cast<uint8_t*>(chunk), reinterpret_cast<uint8_t*>(chunk) + len);
        PQfreemem(chunk);
    }
}

void Reader::finish() {
    if (eof_) return;
    PGconn* conn = static_cast<PGconn*>(conn_);
    // Drain any remaining chunks defensively (a well-formed stream has
    // nothing after the trailer, but don't assume it) until the real -1
    // end-of-copy signal.
    char* chunk = nullptr;
    int len;
    while ((len = PQgetCopyData(conn, &chunk, 0)) >= 0) {
        if (chunk) PQfreemem(chunk);
    }
    eof_ = true;
    if (len == -2) {
        throw std::runtime_error(std::string("PgCopyBinary::Reader: PQgetCopyData error: ") +
                                  PQerrorMessage(conn));
    }
    PGresult* res = PQgetResult(conn);
    ExecStatusType st = PQresultStatus(res);
    std::string err = (st != PGRES_COMMAND_OK) ? PQerrorMessage(conn) : std::string();
    PQclear(res);
    if (st != PGRES_COMMAND_OK)
        throw std::runtime_error("PgCopyBinary::Reader: COPY did not complete cleanly: " + err);
}

std::optional<Row> Reader::next() {
    // mark_ pins the start of this row so fill()'s compaction (should more
    // data need to be pulled partway through parsing it) never erases
    // bytes this row still needs -- see the mark_ field comment in the
    // header for why a plain local start-offset variable wouldn't survive
    // that compaction.
    mark_ = pos_;

    // Row header/field-length prefixes are always network byte order
    // (big-endian) per the COPY binary format spec, independent of any
    // individual field's own payload byte order.
    fill(2);
    int16_t field_count = static_cast<int16_t>((static_cast<uint16_t>(buf_[pos_]) << 8) | buf_[pos_ + 1]);
    pos_ += 2;

    // A field_count of -1 is the trailer, not a legitimate 0-field row.
    if (field_count == -1) {
        finish();
        return std::nullopt;
    }
    if (field_count < 0)
        throw std::runtime_error("PgCopyBinary::Reader: malformed row, negative field count");

    // Row bytes get copied out of buf_ into the Row's own storage (rather
    // than parsing Field views directly against buf_) because buf_ is
    // mutated (compacted/appended) by later fill() calls -- a Field
    // pointing into buf_ would dangle as soon as the next row is read.
    Row row;
    struct Rel { size_t offset; int32_t len; };
    std::vector<Rel> rel_fields;
    rel_fields.reserve(static_cast<size_t>(field_count));

    for (int16_t i = 0; i < field_count; ++i) {
        fill(4);
        const uint8_t* p = buf_.data() + pos_;
        int32_t flen = static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 24) |
                                             (static_cast<uint32_t>(p[1]) << 16) |
                                             (static_cast<uint32_t>(p[2]) << 8) |
                                             static_cast<uint32_t>(p[3]));
        pos_ += 4;
        if (flen == -1) {
            rel_fields.push_back({0, -1});
            continue;
        }
        if (flen < 0)
            throw std::runtime_error("PgCopyBinary::Reader: malformed row, negative field length");
        fill(static_cast<size_t>(flen));
        rel_fields.push_back({pos_ - mark_, flen});
        pos_ += static_cast<size_t>(flen);
    }

    row.raw.assign(buf_.begin() + static_cast<ptrdiff_t>(mark_), buf_.begin() + static_cast<ptrdiff_t>(pos_));
    row.fields.reserve(rel_fields.size());
    for (const auto& r : rel_fields) {
        if (r.len == -1) row.fields.push_back({nullptr, -1});
        else row.fields.push_back({row.raw.data() + r.offset, r.len});
    }
    return row;
}

int64_t readBigintBE(const Field& f) {
    if (f.len != 8 || !f.data)
        throw std::runtime_error("readBigintBE: field is not a non-NULL 8-byte value");
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u = (u << 8) | f.data[i];
    return static_cast<int64_t>(u);
}

Writer::Writer(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("PgCopyBinary::Writer: cannot open " + path);
    f_ = f;

    std::fwrite(kSignature, 1, sizeof(kSignature), f);
    const uint8_t zero4[4] = {0, 0, 0, 0};
    std::fwrite(zero4, 1, 4, f);  // flags
    std::fwrite(zero4, 1, 4, f);  // header extension length
}

Writer::~Writer() {
    if (!closed_ && f_) close();
}

void Writer::writeRow(const std::vector<uint8_t>& raw) {
    if (closed_) throw std::runtime_error("PgCopyBinary::Writer: writeRow after close");
    std::fwrite(raw.data(), 1, raw.size(), static_cast<FILE*>(f_));
}

void Writer::close() {
    if (closed_) return;
    const uint8_t trailer[2] = {0xFF, 0xFF};  // int16(-1), big-endian
    std::fwrite(trailer, 1, 2, static_cast<FILE*>(f_));
    std::fclose(static_cast<FILE*>(f_));
    f_ = nullptr;
    closed_ = true;
}

} // namespace PgCopyBinary
