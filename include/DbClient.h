#pragma once
#include <pqxx/pqxx>
#include <string>
#include "DbConn.h"

// Common base for anything that owns a primary Postgres connection built
// from the standard host/user/database triple every loader's CLI takes.
// Consolidates what used to be five+ independent free-function loader
// files (each hand-building its own connection string) plus NavDB (which
// held its connection via a unique_ptr) into one hierarchy.
//
// Derived classes get a ready-to-use `conn_` for their own single-threaded
// work, plus newConnection() for worker threads that need their own
// connection — pqxx::connection is not safe to share across threads, so
// WMMLoader/TerrainLoader's worker pools call this instead of each
// hand-building a connection string per thread.
class DbClient {
public:
    DbClient(std::string host, std::string user, std::string database)
        : host_(std::move(host)), user_(std::move(user)), database_(std::move(database)),
          conn_(makeConnString(host_, database_, user_)) {}

    virtual ~DbClient() = default;

    DbClient(const DbClient&) = delete;
    DbClient& operator=(const DbClient&) = delete;

protected:
    pqxx::connection newConnection() const {
        return pqxx::connection(makeConnString(host_, database_, user_));
    }

    std::string host_, user_, database_;
    pqxx::connection conn_;
};
