#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace flexql {

struct QueryResult {
    std::vector<std::string> columns;
    std::vector<std::vector<std::optional<std::string>>> rows;
};

struct SessionContext {
    std::string current_database;
};

class Database {
public:
    Database();
    ~Database();

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    bool execute(SessionContext &session, const std::string &sql, QueryResult &result, std::string &error);
    bool execute(const std::string &sql, QueryResult &result, std::string &error);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace flexql
