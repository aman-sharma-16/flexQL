#include "flexql.h"

#include <cctype>
#include <cstring>
#include <iostream>
#include <string>

namespace {

std::string trim(const std::string &value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string toUpper(std::string value) {
    for (char &c : value) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return value;
}

struct CallbackState {
    bool printed_any_rows = false;
};

int printRow(void *data, int columnCount, char **values, char **columnNames) {
    auto *state = static_cast<CallbackState *>(data);
    state->printed_any_rows = true;

    for (int i = 0; i < columnCount; ++i) {
        std::cout << columnNames[i] << " = " << (values[i] ? values[i] : "NULL") << '\n';
    }
    std::cout << '\n';
    return 0;
}

bool isCompleteStatement(const std::string &sql) {
    bool in_string = false;
    for (std::size_t i = 0; i < sql.size(); ++i) {
        if (sql[i] == '\'') {
            if (in_string && i + 1 < sql.size() && sql[i + 1] == '\'') {
                ++i;
                continue;
            }
            in_string = !in_string;
        }
    }

    return !in_string && sql.find(';') != std::string::npos;
}

}  // namespace

int main(int argc, char **argv) {
    const char *host = argc >= 2 ? argv[1] : "127.0.0.1";
    const int port = argc >= 3 ? std::stoi(argv[2]) : 9000;

    FlexQL *db = nullptr;
    if (flexql_open(host, port, &db) != FLEXQL_OK) {
        std::cerr << "Cannot connect to FlexQL server\n";
        return 1;
    }

    std::cout << "Connected to FlexQL server\n";

    std::string pending_sql;
    std::string line;
    while (true) {
        std::cout << (pending_sql.empty() ? "flexql> " : "    ... ") << std::flush;
        if (!std::getline(std::cin, line)) {
            break;
        }

        const std::string stripped = trim(line);
        if (pending_sql.empty() && (stripped == ".exit" || stripped == "exit")) {
            break;
        }

        if (!pending_sql.empty()) {
            pending_sql.push_back('\n');
        }
        pending_sql += line;

        if (!isCompleteStatement(pending_sql)) {
            continue;
        }

        CallbackState state;
        char *errmsg = nullptr;
        const int rc = flexql_exec(db, pending_sql.c_str(), printRow, &state, &errmsg);
        if (rc != FLEXQL_OK) {
            std::cerr << "SQL error: " << (errmsg ? errmsg : "unknown error") << '\n';
            if (errmsg) {
                flexql_free(errmsg);
            }
        } else if (!state.printed_any_rows) {
            const std::string upper = toUpper(trim(pending_sql));
            if (upper.rfind("SELECT", 0) == 0) {
                std::cout << "(no rows)\n";
            } else {
                std::cout << "OK\n";
            }
        }

        pending_sql.clear();
    }

    flexql_close(db);
    std::cout << "Connection closed\n";
    return 0;
}
