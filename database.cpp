#include "database.h"

#include "indexes.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace flexql {
namespace {

namespace fs = std::filesystem;

constexpr const char *kDefaultDatabaseName = "DEFAULT";
constexpr const char *kDataRoot = "flexql_data";
constexpr const char *kSchemaExtension = ".schema";
constexpr const char *kRowsExtension = ".rows";
constexpr const char kBinaryRowsMagic[] = "FQLB1\n";
constexpr std::size_t kBinaryRowsMagicSize = sizeof(kBinaryRowsMagic) - 1;
constexpr std::uint32_t kNullFieldLength = 0xFFFFFFFFU;

enum class ColumnType {
    Int,
    Decimal,
    Varchar,
    DateTime,
};

struct ColumnSchema {
    std::string name;
    ColumnType type;
    bool primary_key = false;
    bool not_null = false;
};

struct RowRef {
    std::uint64_t offset = 0;
    long long expires_at_unix_seconds = -1;
};

struct LoadedRow {
    std::vector<std::optional<std::string>> values;
    std::optional<std::chrono::system_clock::time_point> expires_at;
};

enum class RowStorageFormat {
    LegacyText,
    BinaryV1,
};

struct Table {
    std::string name;
    std::vector<ColumnSchema> columns;
    std::unordered_map<std::string, std::size_t> column_lookup;
    std::vector<RowRef> rows;
    std::optional<std::size_t> primary_key_column;
    PrimaryIndex primary_index;
    fs::path schema_path;
    fs::path rows_path;
    std::string cache_namespace;
    int append_fd = -1;
    int read_fd = -1;
    std::uint64_t next_offset = 0;
    RowStorageFormat row_format = RowStorageFormat::BinaryV1;
};

struct DatabaseState {
    std::string name;
    fs::path path;
    std::unordered_map<std::string, Table> tables;
};

struct Token {
    enum class Kind {
        Word,
        String,
        Symbol,
        Operator,
    };

    Kind kind;
    std::string text;
};

struct ColumnRef {
    std::string raw;
    std::string table;
    std::string column;
    bool qualified = false;
};

struct Literal {
    bool is_null = false;
    std::string text;
};

struct Operand {
    bool is_identifier = false;
    bool is_null = false;
    std::string text;
    ColumnRef identifier;
};

struct Condition {
    ColumnRef left;
    std::string op;
    Operand right;
};

struct JoinClause {
    std::string table_name;
    ColumnRef left;
    ColumnRef right;
};

struct CreateDatabaseStatement {
    std::string database_name;
    bool if_not_exists = false;
};

struct UseStatement {
    std::string database_name;
};

struct CreateTableStatement {
    std::string table_name;
    std::vector<ColumnSchema> columns;
    bool if_not_exists = false;
};

struct InsertStatement {
    std::string table_name;
    std::vector<std::vector<Literal>> rows;
    std::optional<std::chrono::system_clock::time_point> expires_at;
};

struct SelectStatement {
    bool select_all = false;
    std::vector<ColumnRef> columns;
    std::string from_table;
    std::optional<JoinClause> join;
    std::optional<Condition> where;
    std::string cache_key;
};

using Statement =
    std::variant<CreateDatabaseStatement, UseStatement, CreateTableStatement, InsertStatement, SelectStatement>;

std::string trim(const std::string &value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string toUpper(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); }
    );
    return value;
}

std::string normalizeIdentifier(const std::string &value) {
    return toUpper(value);
}

std::string normalizeSql(const std::string &sql) {
    std::string normalized;
    normalized.reserve(sql.size());

    bool in_string = false;
    bool previous_space = false;

    for (std::size_t i = 0; i < sql.size(); ++i) {
        const char c = sql[i];

        if (c == '\'') {
            normalized.push_back(c);
            if (in_string && i + 1 < sql.size() && sql[i + 1] == '\'') {
                normalized.push_back(sql[i + 1]);
                ++i;
                continue;
            }
            in_string = !in_string;
            previous_space = false;
            continue;
        }

        if (!in_string && std::isspace(static_cast<unsigned char>(c))) {
            if (!previous_space && !normalized.empty()) {
                normalized.push_back(' ');
                previous_space = true;
            }
            continue;
        }

        normalized.push_back(c);
        previous_space = false;
    }

    normalized = trim(normalized);
    if (!normalized.empty() && normalized.back() == ';') {
        normalized.pop_back();
    }
    return trim(normalized);
}

bool isWordChar(char c) {
    return !std::isspace(static_cast<unsigned char>(c)) &&
           c != '(' &&
           c != ')' &&
           c != ',' &&
           c != ';' &&
           c != '*' &&
           c != '\'' &&
           c != '<' &&
           c != '>' &&
           c != '=' &&
           c != '!';
}

std::vector<Token> tokenize(const std::string &sql) {
    std::vector<Token> tokens;

    for (std::size_t i = 0; i < sql.size();) {
        const char c = sql[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }

        if (c == '\'') {
            std::string value;
            ++i;
            while (i < sql.size()) {
                if (sql[i] == '\'') {
                    if (i + 1 < sql.size() && sql[i + 1] == '\'') {
                        value.push_back('\'');
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                value.push_back(sql[i]);
                ++i;
            }
            tokens.push_back({Token::Kind::String, value});
            continue;
        }

        if (c == '(' || c == ')' || c == ',' || c == ';' || c == '*') {
            tokens.push_back({Token::Kind::Symbol, std::string(1, c)});
            ++i;
            continue;
        }

        if (c == '<' || c == '>' || c == '=' || c == '!') {
            std::string op(1, c);
            if (i + 1 < sql.size() && sql[i + 1] == '=') {
                op.push_back('=');
                ++i;
            }
            tokens.push_back({Token::Kind::Operator, op});
            ++i;
            continue;
        }

        const std::size_t start = i;
        while (i < sql.size() && isWordChar(sql[i])) {
            ++i;
        }
        tokens.push_back({Token::Kind::Word, sql.substr(start, i - start)});
    }

    return tokens;
}

bool parseIntegerStrict(const std::string &text, long long &value) {
    try {
        std::size_t parsed = 0;
        value = std::stoll(text, &parsed);
        return parsed == text.size();
    } catch (...) {
        return false;
    }
}

bool parseDecimalStrict(const std::string &text, long double &value) {
    try {
        std::size_t parsed = 0;
        value = std::stold(text, &parsed);
        return parsed == text.size();
    } catch (...) {
        return false;
    }
}

bool looksLikeQualifiedIdentifier(const std::string &text) {
    if (text.find('.') == std::string::npos) {
        return false;
    }

    long double numeric = 0;
    return !parseDecimalStrict(text, numeric);
}

std::optional<std::chrono::system_clock::time_point> parseDateTime(const std::string &text) {
    long long unix_seconds = 0;
    if (parseIntegerStrict(text, unix_seconds)) {
        return std::chrono::system_clock::from_time_t(static_cast<std::time_t>(unix_seconds));
    }

    const char *formats[] = {
        "%Y-%m-%d %H:%M:%S",
        "%Y-%m-%dT%H:%M:%S",
        "%Y-%m-%d",
    };

    for (const char *format : formats) {
        std::tm tm = {};
        std::istringstream stream(text);
        stream >> std::get_time(&tm, format);
        if (!stream.fail()) {
            tm.tm_isdst = -1;
            const std::time_t time_value = std::mktime(&tm);
            if (time_value != -1) {
                return std::chrono::system_clock::from_time_t(time_value);
            }
        }
    }

    return std::nullopt;
}

std::string formatDateTime(const std::chrono::system_clock::time_point &time_point) {
    const std::time_t time_value = std::chrono::system_clock::to_time_t(time_point);
    const std::tm *tm = std::localtime(&time_value);

    std::ostringstream output;
    output << std::put_time(tm, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

long long timePointToUnixSeconds(const std::chrono::system_clock::time_point &time_point) {
    return static_cast<long long>(std::chrono::system_clock::to_time_t(time_point));
}

std::optional<std::chrono::system_clock::time_point> unixSecondsToTimePoint(long long value) {
    if (value < 0) {
        return std::nullopt;
    }
    return std::chrono::system_clock::from_time_t(static_cast<std::time_t>(value));
}

std::string normalizeDecimal(long double value) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(12) << value;
    std::string text = output.str();
    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    if (text.empty()) {
        return "0";
    }
    return text;
}

ColumnType parseColumnType(const std::string &text) {
    const std::string upper = toUpper(text);
    if (upper == "INT" || upper == "INTEGER") {
        return ColumnType::Int;
    }
    if (upper == "DECIMAL" || upper == "FLOAT" || upper == "DOUBLE") {
        return ColumnType::Decimal;
    }
    if (upper == "VARCHAR" || upper == "TEXT" || upper == "STRING") {
        return ColumnType::Varchar;
    }
    if (upper == "DATETIME" || upper == "TIMESTAMP") {
        return ColumnType::DateTime;
    }
    throw std::runtime_error("Unsupported column type: " + text);
}

std::string columnTypeToString(ColumnType type) {
    switch (type) {
        case ColumnType::Int:
            return "INT";
        case ColumnType::Decimal:
            return "DECIMAL";
        case ColumnType::Varchar:
            return "VARCHAR";
        case ColumnType::DateTime:
            return "DATETIME";
    }
    throw std::runtime_error("Unsupported column type");
}

std::optional<std::string> normalizeLiteralForType(
    ColumnType type,
    const Literal &literal,
    bool not_null
) {
    if (literal.is_null) {
        if (not_null) {
            throw std::runtime_error("NULL is not allowed for this column");
        }
        return std::nullopt;
    }

    switch (type) {
        case ColumnType::Int: {
            long long value = 0;
            if (!parseIntegerStrict(literal.text, value)) {
                throw std::runtime_error("Invalid INT value: " + literal.text);
            }
            return std::to_string(value);
        }
        case ColumnType::Decimal: {
            long double value = 0;
            if (!parseDecimalStrict(literal.text, value)) {
                throw std::runtime_error("Invalid DECIMAL value: " + literal.text);
            }
            return normalizeDecimal(value);
        }
        case ColumnType::Varchar:
            return literal.text;
        case ColumnType::DateTime: {
            const auto parsed = parseDateTime(literal.text);
            if (!parsed.has_value()) {
                throw std::runtime_error("Invalid DATETIME value: " + literal.text);
            }
            return formatDateTime(*parsed);
        }
    }

    throw std::runtime_error("Unsupported column type");
}

int compareTypedValues(ColumnType type, const std::string &left, const std::string &right) {
    switch (type) {
        case ColumnType::Int: {
            long long left_value = 0;
            long long right_value = 0;
            parseIntegerStrict(left, left_value);
            parseIntegerStrict(right, right_value);
            if (left_value < right_value) {
                return -1;
            }
            if (left_value > right_value) {
                return 1;
            }
            return 0;
        }
        case ColumnType::Decimal: {
            long double left_value = 0;
            long double right_value = 0;
            parseDecimalStrict(left, left_value);
            parseDecimalStrict(right, right_value);
            if (left_value < right_value) {
                return -1;
            }
            if (left_value > right_value) {
                return 1;
            }
            return 0;
        }
        case ColumnType::DateTime:
        case ColumnType::Varchar:
            if (left < right) {
                return -1;
            }
            if (left > right) {
                return 1;
            }
            return 0;
    }

    return 0;
}

bool applyComparison(
    ColumnType type,
    const std::optional<std::string> &left,
    const std::optional<std::string> &right,
    const std::string &op
) {
    if (!left.has_value() || !right.has_value()) {
        if (op == "=") {
            return left == right;
        }
        if (op == "!=") {
            return left != right;
        }
        return false;
    }

    const int comparison = compareTypedValues(type, *left, *right);
    if (op == "=") {
        return comparison == 0;
    }
    if (op == "!=") {
        return comparison != 0;
    }
    if (op == "<") {
        return comparison < 0;
    }
    if (op == "<=") {
        return comparison <= 0;
    }
    if (op == ">") {
        return comparison > 0;
    }
    if (op == ">=") {
        return comparison >= 0;
    }
    return false;
}

bool isExpired(long long expires_at_unix_seconds) {
    if (expires_at_unix_seconds < 0) {
        return false;
    }
    return std::time(nullptr) >= expires_at_unix_seconds;
}

std::string hexEncode(const std::string &value) {
    static const char *digits = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 2);
    for (unsigned char c : value) {
        encoded.push_back(digits[c >> 4]);
        encoded.push_back(digits[c & 0x0F]);
    }
    return encoded;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

std::string hexDecode(const std::string &value) {
    if (value.size() % 2 != 0) {
        throw std::runtime_error("Corrupt persisted data");
    }

    std::string decoded;
    decoded.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const int high = hexValue(value[i]);
        const int low = hexValue(value[i + 1]);
        if (high < 0 || low < 0) {
            throw std::runtime_error("Corrupt persisted data");
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    return decoded;
}

std::vector<std::string> split(const std::string &value, char delimiter) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : value) {
        if (c == delimiter) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    return parts;
}

void syncDirectory(const fs::path &path) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return;
    }
    fsync(fd);
    close(fd);
}

void ensureDirectory(const fs::path &path) {
    std::error_code error;
    if (fs::exists(path, error)) {
        if (!fs::is_directory(path, error)) {
            throw std::runtime_error("Path is not a directory: " + path.string());
        }
        return;
    }

    if (!fs::create_directories(path, error) && error) {
        throw std::runtime_error("Failed to create directory: " + path.string());
    }

    const fs::path parent = path.parent_path().empty() ? fs::path(".") : path.parent_path();
    syncDirectory(parent);
    syncDirectory(path);
}

void writeAll(int fd, const std::string &data) {
    std::size_t total = 0;
    while (total < data.size()) {
        const ssize_t written = write(fd, data.data() + total, data.size() - total);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("Persistent write failed");
        }
        total += static_cast<std::size_t>(written);
    }
}

void durableFileSync(int fd) {
#if defined(__linux__)
    if (fdatasync(fd) != 0) {
        throw std::runtime_error("Failed to flush persistent table data");
    }
#else
    if (fsync(fd) != 0) {
        throw std::runtime_error("Failed to flush persistent table data");
    }
#endif
}

void appendUint32LE(std::string &buffer, std::uint32_t value) {
    buffer.push_back(static_cast<char>(value & 0xFFU));
    buffer.push_back(static_cast<char>((value >> 8) & 0xFFU));
    buffer.push_back(static_cast<char>((value >> 16) & 0xFFU));
    buffer.push_back(static_cast<char>((value >> 24) & 0xFFU));
}

void appendInt64LE(std::string &buffer, std::int64_t value) {
    const std::uint64_t raw = static_cast<std::uint64_t>(value);
    for (int shift = 0; shift < 64; shift += 8) {
        buffer.push_back(static_cast<char>((raw >> shift) & 0xFFU));
    }
}

bool decodeUint32LE(const char *data, std::size_t size, std::size_t &offset, std::uint32_t &value) {
    if (offset + 4 > size) {
        return false;
    }
    value = static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset])) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 8) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 3])) << 24);
    offset += 4;
    return true;
}

bool decodeInt64LE(const char *data, std::size_t size, std::size_t &offset, std::int64_t &value) {
    if (offset + 8 > size) {
        return false;
    }

    std::uint64_t raw = 0;
    for (int i = 0; i < 8; ++i) {
        raw |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[offset + i])) << (i * 8);
    }
    offset += 8;
    value = static_cast<std::int64_t>(raw);
    return true;
}

void touchFile(const fs::path &path) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        throw std::runtime_error("Failed to create file: " + path.string());
    }
    if (fsync(fd) != 0) {
        close(fd);
        throw std::runtime_error("Failed to fsync file: " + path.string());
    }
    close(fd);
}

void writeFileAtomically(const fs::path &path, const std::string &data) {
    const fs::path temp_path = path.string() + ".tmp";

    const int fd = open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        throw std::runtime_error("Failed to open temp file: " + temp_path.string());
    }

    try {
        writeAll(fd, data);
        if (fsync(fd) != 0) {
            throw std::runtime_error("Failed to fsync temp file: " + temp_path.string());
        }
        close(fd);
    } catch (...) {
        close(fd);
        std::error_code ignored;
        fs::remove(temp_path, ignored);
        throw;
    }

    std::error_code error;
    fs::rename(temp_path, path, error);
    if (error) {
        std::error_code ignored;
        fs::remove(temp_path, ignored);
        throw std::runtime_error("Failed to replace file: " + path.string());
    }

    syncDirectory(path.parent_path().empty() ? fs::path(".") : path.parent_path());
}

std::string serializeSchema(const Table &table) {
    std::ostringstream output;
    output << "TABLE\t" << hexEncode(table.name) << '\n';
    for (const ColumnSchema &column : table.columns) {
        output << "COLUMN\t" << hexEncode(column.name) << '\t' << columnTypeToString(column.type) << '\t'
               << (column.primary_key ? 1 : 0) << '\t' << (column.not_null ? 1 : 0) << '\n';
    }
    return output.str();
}

LoadedRow deserializeRowLine(const std::string &line, std::size_t expected_columns) {
    const std::vector<std::string> parts = split(line, '\t');
    if (parts.size() < 2 || parts[0] != "ROW") {
        throw std::runtime_error("Corrupt row record");
    }

    long long expires_seconds = -1;
    if (!parseIntegerStrict(parts[1], expires_seconds)) {
        throw std::runtime_error("Corrupt row expiration");
    }

    LoadedRow row;
    row.expires_at = unixSecondsToTimePoint(expires_seconds);
    row.values.reserve(expected_columns);

    if (parts.size() - 2 != expected_columns) {
        throw std::runtime_error("Row width does not match table schema");
    }

    for (std::size_t i = 2; i < parts.size(); ++i) {
        if (parts[i] == "N") {
            row.values.push_back(std::nullopt);
            continue;
        }
        if (parts[i].empty() || parts[i][0] != 'V') {
            throw std::runtime_error("Corrupt row value");
        }
        row.values.push_back(hexDecode(parts[i].substr(1)));
    }

    return row;
}

std::string serializeRowLine(const LoadedRow &row) {
    std::ostringstream output;
    output << "ROW\t" << (row.expires_at.has_value() ? timePointToUnixSeconds(*row.expires_at) : -1);
    for (const auto &value : row.values) {
        output << '\t';
        if (!value.has_value()) {
            output << 'N';
        } else {
            output << 'V' << hexEncode(*value);
        }
    }
    output << '\n';
    return output.str();
}

LoadedRow deserializeBinaryRowRecord(const char *data, std::size_t size, std::size_t expected_columns) {
    std::size_t offset = 0;
    std::int64_t expires_seconds = -1;
    if (!decodeInt64LE(data, size, offset, expires_seconds)) {
        throw std::runtime_error("Corrupt binary row expiration");
    }

    LoadedRow row;
    row.expires_at = unixSecondsToTimePoint(static_cast<long long>(expires_seconds));
    row.values.reserve(expected_columns);

    for (std::size_t i = 0; i < expected_columns; ++i) {
        std::uint32_t field_length = 0;
        if (!decodeUint32LE(data, size, offset, field_length)) {
            throw std::runtime_error("Corrupt binary row value");
        }

        if (field_length == kNullFieldLength) {
            row.values.push_back(std::nullopt);
            continue;
        }

        if (offset + field_length > size) {
            throw std::runtime_error("Corrupt binary row payload");
        }

        row.values.push_back(std::string(data + offset, data + offset + field_length));
        offset += field_length;
    }

    if (offset != size) {
        throw std::runtime_error("Corrupt binary row tail");
    }

    return row;
}

std::string serializeBinaryRowRecord(const LoadedRow &row) {
    std::string payload;
    payload.reserve(64 + row.values.size() * 16);

    appendInt64LE(
        payload,
        static_cast<std::int64_t>(row.expires_at.has_value() ? timePointToUnixSeconds(*row.expires_at) : -1)
    );

    for (const auto &value : row.values) {
        if (!value.has_value()) {
            appendUint32LE(payload, kNullFieldLength);
            continue;
        }

        appendUint32LE(payload, static_cast<std::uint32_t>(value->size()));
        payload.append(*value);
    }

    std::string record;
    record.reserve(4 + payload.size());
    appendUint32LE(record, static_cast<std::uint32_t>(payload.size()));
    record += payload;
    return record;
}

bool hasBinaryRowsMagic(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }

    char magic[kBinaryRowsMagicSize] = {};
    input.read(magic, static_cast<std::streamsize>(kBinaryRowsMagicSize));
    if (input.gcount() == 0) {
        return false;
    }
    if (static_cast<std::size_t>(input.gcount()) != kBinaryRowsMagicSize) {
        return false;
    }
    return std::memcmp(magic, kBinaryRowsMagic, kBinaryRowsMagicSize) == 0;
}

std::string makeRowCacheKey(const Table &table, std::uint64_t offset) {
    return table.cache_namespace + "#" + std::to_string(offset);
}

class Parser {
public:
    explicit Parser(const std::string &sql) : tokens_(tokenize(sql)) {}

    Statement parse() {
        if (peekKeyword("CREATE")) {
            return parseCreate();
        }
        if (peekKeyword("USE")) {
            return parseUse();
        }
        if (peekKeyword("INSERT")) {
            return parseInsert();
        }
        if (peekKeyword("SELECT")) {
            return parseSelect();
        }
        throw std::runtime_error("Unsupported SQL statement");
    }

private:
    Statement parseCreate() {
        expectKeyword("CREATE");
        if (peekKeyword("DATABASE")) {
            return parseCreateDatabase();
        }
        if (peekKeyword("TABLE")) {
            return parseCreateTable();
        }
        throw std::runtime_error("Unsupported CREATE statement");
    }

    CreateDatabaseStatement parseCreateDatabase() {
        expectKeyword("DATABASE");

        CreateDatabaseStatement statement;
        statement.if_not_exists = parseIfNotExists();
        statement.database_name = expectWord("database name").text;

        matchSymbol(";");
        ensureFinished();
        return statement;
    }

    UseStatement parseUse() {
        expectKeyword("USE");

        UseStatement statement;
        statement.database_name = expectWord("database name").text;

        matchSymbol(";");
        ensureFinished();
        return statement;
    }

    CreateTableStatement parseCreateTable() {
        expectKeyword("TABLE");

        CreateTableStatement statement;
        statement.if_not_exists = parseIfNotExists();
        statement.table_name = expectWord("table name").text;

        expectSymbol("(");
        bool seen_primary_key = false;

        while (true) {
            ColumnSchema column;
            column.name = expectWord("column name").text;
            column.type = parseTypeDefinition();

            while (peekWord()) {
                if (peekKeyword("PRIMARY")) {
                    expectKeyword("PRIMARY");
                    expectKeyword("KEY");
                    if (seen_primary_key) {
                        throw std::runtime_error("Only one PRIMARY KEY is supported");
                    }
                    column.primary_key = true;
                    column.not_null = true;
                    seen_primary_key = true;
                    continue;
                }
                if (peekKeyword("NOT")) {
                    expectKeyword("NOT");
                    expectKeyword("NULL");
                    column.not_null = true;
                    continue;
                }
                break;
            }

            statement.columns.push_back(column);

            if (matchSymbol(")")) {
                break;
            }
            expectSymbol(",");
        }

        matchSymbol(";");
        ensureFinished();
        return statement;
    }

    InsertStatement parseInsert() {
        expectKeyword("INSERT");
        expectKeyword("INTO");

        InsertStatement statement;
        statement.table_name = expectWord("table name").text;

        expectKeyword("VALUES");
        while (true) {
            std::vector<Literal> row_values;
            expectSymbol("(");
            while (true) {
                row_values.push_back(parseLiteral());
                if (matchSymbol(")")) {
                    break;
                }
                expectSymbol(",");
            }
            statement.rows.push_back(std::move(row_values));

            if (!matchSymbol(",")) {
                break;
            }
        }

        if (peekKeyword("EXPIRES")) {
            expectKeyword("EXPIRES");
            const Literal literal = parseLiteral();
            if (!literal.is_null) {
                const auto expires_at = parseDateTime(literal.text);
                if (!expires_at.has_value()) {
                    throw std::runtime_error("Invalid expiration timestamp");
                }
                statement.expires_at = expires_at;
            }
        } else if (peekKeyword("TTL")) {
            expectKeyword("TTL");
            const Token ttl_token = expectWord("TTL seconds");
            long long ttl_seconds = 0;
            if (!parseIntegerStrict(ttl_token.text, ttl_seconds) || ttl_seconds < 0) {
                throw std::runtime_error("TTL must be a non-negative integer");
            }
            statement.expires_at =
                std::chrono::system_clock::now() + std::chrono::seconds(ttl_seconds);
        }

        matchSymbol(";");
        ensureFinished();
        return statement;
    }

    SelectStatement parseSelect() {
        expectKeyword("SELECT");

        SelectStatement statement;
        if (matchSymbol("*")) {
            statement.select_all = true;
        } else {
            while (true) {
                statement.columns.push_back(parseColumnRef());
                if (peekKeyword("FROM")) {
                    break;
                }
                expectSymbol(",");
            }
        }

        expectKeyword("FROM");
        statement.from_table = expectWord("table name").text;

        if (peekKeyword("INNER")) {
            JoinClause join;
            expectKeyword("INNER");
            expectKeyword("JOIN");
            join.table_name = expectWord("join table name").text;
            expectKeyword("ON");
            join.left = parseColumnRef();
            expectJoinOperator();
            join.right = parseColumnRef();
            statement.join = join;
        }

        if (peekKeyword("WHERE")) {
            expectKeyword("WHERE");
            statement.where = parseCondition();
        }

        matchSymbol(";");
        ensureFinished();
        return statement;
    }

    ColumnType parseTypeDefinition() {
        const Token type_token = expectWord("column type");
        ColumnType type = parseColumnType(type_token.text);

        if (matchSymbol("(")) {
            int depth = 1;
            while (depth > 0) {
                const Token token = consume("type modifier");
                if (token.kind == Token::Kind::Symbol && token.text == "(") {
                    ++depth;
                } else if (token.kind == Token::Kind::Symbol && token.text == ")") {
                    --depth;
                }
            }
        }

        return type;
    }

    bool parseIfNotExists() {
        if (!peekKeyword("IF")) {
            return false;
        }
        expectKeyword("IF");
        expectKeyword("NOT");
        expectKeyword("EXISTS");
        return true;
    }

    void expectJoinOperator() {
        const Token op = expectOperator("join operator");
        if (op.text != "=") {
            throw std::runtime_error("INNER JOIN only supports equality conditions");
        }
    }

    Condition parseCondition() {
        Condition condition;
        condition.left = parseColumnRef();
        condition.op = expectOperator("comparison operator").text;

        const Token value = consume("condition value");
        if (value.kind == Token::Kind::String) {
            condition.right.text = value.text;
        } else if (value.kind == Token::Kind::Word) {
            if (toUpper(value.text) == "NULL") {
                condition.right.is_null = true;
            } else if (looksLikeQualifiedIdentifier(value.text)) {
                condition.right.is_identifier = true;
                condition.right.identifier = splitColumnRef(value.text);
            } else {
                condition.right.text = value.text;
            }
        } else {
            throw std::runtime_error("Invalid condition value");
        }

        return condition;
    }

    Literal parseLiteral() {
        const Token token = consume("value");
        if (token.kind == Token::Kind::String) {
            return {false, token.text};
        }
        if (token.kind == Token::Kind::Word) {
            if (toUpper(token.text) == "NULL") {
                return {true, ""};
            }
            return {false, token.text};
        }
        throw std::runtime_error("Expected literal value");
    }

    ColumnRef parseColumnRef() {
        return splitColumnRef(expectWord("column reference").text);
    }

    ColumnRef splitColumnRef(const std::string &text) const {
        ColumnRef ref;
        ref.raw = text;
        const auto dot = text.find('.');
        if (dot == std::string::npos) {
            ref.column = text;
            return ref;
        }
        ref.qualified = true;
        ref.table = text.substr(0, dot);
        ref.column = text.substr(dot + 1);
        if (ref.table.empty() || ref.column.empty()) {
            throw std::runtime_error("Invalid column reference: " + text);
        }
        return ref;
    }

    Token consume(const std::string &expected) {
        if (position_ >= tokens_.size()) {
            throw std::runtime_error("Expected " + expected + " but reached end of input");
        }
        return tokens_[position_++];
    }

    Token expectWord(const std::string &expected) {
        const Token token = consume(expected);
        if (token.kind != Token::Kind::Word) {
            throw std::runtime_error("Expected " + expected);
        }
        return token;
    }

    Token expectOperator(const std::string &expected) {
        const Token token = consume(expected);
        if (token.kind != Token::Kind::Operator) {
            throw std::runtime_error("Expected " + expected);
        }
        return token;
    }

    void expectKeyword(const std::string &keyword) {
        const Token token = expectWord(keyword);
        if (toUpper(token.text) != keyword) {
            throw std::runtime_error("Expected keyword " + keyword);
        }
    }

    void expectSymbol(const std::string &symbol) {
        const Token token = consume(symbol);
        if (token.kind != Token::Kind::Symbol || token.text != symbol) {
            throw std::runtime_error("Expected symbol " + symbol);
        }
    }

    bool matchSymbol(const std::string &symbol) {
        if (position_ < tokens_.size() &&
            tokens_[position_].kind == Token::Kind::Symbol &&
            tokens_[position_].text == symbol) {
            ++position_;
            return true;
        }
        return false;
    }

    bool peekKeyword(const std::string &keyword) const {
        return position_ < tokens_.size() &&
               tokens_[position_].kind == Token::Kind::Word &&
               toUpper(tokens_[position_].text) == keyword;
    }

    bool peekWord() const {
        return position_ < tokens_.size() && tokens_[position_].kind == Token::Kind::Word;
    }

    void ensureFinished() {
        if (position_ != tokens_.size()) {
            throw std::runtime_error("Unexpected trailing tokens");
        }
    }

    std::vector<Token> tokens_;
    std::size_t position_ = 0;
};

struct ResolvedColumn {
    const Table *table = nullptr;
    const ColumnSchema *schema = nullptr;
    std::size_t index = 0;
    bool from_right = false;
    std::string output_name;
};

class QueryCache {
public:
    explicit QueryCache(std::size_t capacity) : capacity_(capacity) {}

    bool get(const std::string &key, QueryResult &result) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lookup_.find(key);
        if (it == lookup_.end()) {
            return false;
        }
        entries_.splice(entries_.begin(), entries_, it->second);
        result = it->second->result;
        return true;
    }

    void put(const std::string &key, const QueryResult &result) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (capacity_ == 0) {
            return;
        }

        auto it = lookup_.find(key);
        if (it != lookup_.end()) {
            it->second->result = result;
            entries_.splice(entries_.begin(), entries_, it->second);
            return;
        }

        entries_.push_front({key, result});
        lookup_[key] = entries_.begin();

        if (entries_.size() > capacity_) {
            const auto &last = entries_.back();
            lookup_.erase(last.key);
            entries_.pop_back();
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        lookup_.clear();
    }

private:
    struct Entry {
        std::string key;
        QueryResult result;
    };

    std::size_t capacity_;
    std::mutex mutex_;
    std::list<Entry> entries_;
    std::unordered_map<std::string, std::list<Entry>::iterator> lookup_;
};

class RowCache {
public:
    explicit RowCache(std::size_t capacity) : capacity_(capacity) {}

    bool get(const std::string &key, LoadedRow &row) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lookup_.find(key);
        if (it == lookup_.end()) {
            return false;
        }
        entries_.splice(entries_.begin(), entries_, it->second);
        row = it->second->row;
        return true;
    }

    void put(const std::string &key, const LoadedRow &row) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (capacity_ == 0) {
            return;
        }

        auto it = lookup_.find(key);
        if (it != lookup_.end()) {
            it->second->row = row;
            entries_.splice(entries_.begin(), entries_, it->second);
            return;
        }

        entries_.push_front({key, row});
        lookup_[key] = entries_.begin();

        if (entries_.size() > capacity_) {
            const auto &last = entries_.back();
            lookup_.erase(last.key);
            entries_.pop_back();
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        lookup_.clear();
    }

private:
    struct Entry {
        std::string key;
        LoadedRow row;
    };

    std::size_t capacity_;
    std::mutex mutex_;
    std::list<Entry> entries_;
    std::unordered_map<std::string, std::list<Entry>::iterator> lookup_;
};

}  // namespace

class Database::Impl {
public:
    Impl() : data_root_(fs::absolute(kDataRoot)) {
        loadFromDisk();
    }

    ~Impl() {
        closeAllTableFiles();
    }

    bool execute(SessionContext &session, const std::string &sql, QueryResult &result, std::string &error) {
        result = QueryResult{};
        error.clear();

        Statement statement;
        try {
            statement = Parser(sql).parse();
        } catch (const std::exception &ex) {
            error = ex.what();
            return false;
        }

        try {
            if (std::holds_alternative<CreateDatabaseStatement>(statement)) {
                executeCreateDatabase(std::get<CreateDatabaseStatement>(statement));
                return true;
            }

            if (std::holds_alternative<UseStatement>(statement)) {
                executeUse(session, std::get<UseStatement>(statement));
                return true;
            }

            if (std::holds_alternative<CreateTableStatement>(statement)) {
                executeCreateTable(session, std::get<CreateTableStatement>(statement));
                return true;
            }

            if (std::holds_alternative<InsertStatement>(statement)) {
                executeInsert(session, std::get<InsertStatement>(statement));
                return true;
            }

            SelectStatement select = std::get<SelectStatement>(statement);
            const std::string database_key = currentDatabaseKey(session);
            select.cache_key = database_key + ":" + normalizeSql(sql);

            if (cache_.get(select.cache_key, result)) {
                return true;
            }

            executeSelect(session, select, result);
            cache_.put(select.cache_key, result);
            return true;
        } catch (const std::exception &ex) {
            error = ex.what();
            return false;
        }
    }

private:
    void loadFromDisk() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        closeAllTableFilesUnlocked();
        cache_.clear();
        row_cache_.clear();
        ensureDirectory(data_root_);
        databases_.clear();

        std::error_code error;
        for (const auto &entry : fs::directory_iterator(data_root_, error)) {
            if (error) {
                throw std::runtime_error("Failed to enumerate data directory");
            }
            if (!entry.is_directory()) {
                continue;
            }

            DatabaseState database;
            database.name = entry.path().filename().string();
            database.path = entry.path();
            loadDatabaseTables(database);
            databases_.emplace(normalizeIdentifier(database.name), std::move(database));
        }

        if (databases_.find(normalizeIdentifier(kDefaultDatabaseName)) == databases_.end()) {
            DatabaseState database;
            database.name = kDefaultDatabaseName;
            database.path = data_root_ / kDefaultDatabaseName;
            createDatabaseOnDisk(database.name, database.path);
            databases_.emplace(normalizeIdentifier(database.name), std::move(database));
        }
    }

    void loadDatabaseTables(DatabaseState &database) {
        std::error_code error;
        for (const auto &entry : fs::directory_iterator(database.path, error)) {
            if (error) {
                throw std::runtime_error("Failed to enumerate database directory: " + database.path.string());
            }
            if (!entry.is_regular_file() || entry.path().extension() != kSchemaExtension) {
                continue;
            }

            Table table = loadTableFromDisk(entry.path());
            database.tables.emplace(normalizeIdentifier(table.name), std::move(table));
        }
    }

    Table loadTableFromDisk(const fs::path &schema_path) {
        std::ifstream input(schema_path);
        if (!input) {
            throw std::runtime_error("Failed to open schema file: " + schema_path.string());
        }

        Table table;
        table.schema_path = schema_path;
        table.rows_path = schema_path.parent_path() / (schema_path.stem().string() + kRowsExtension);

        std::string line;
        bool saw_table = false;
        while (std::getline(input, line)) {
            if (line.empty()) {
                continue;
            }
            const std::vector<std::string> parts = split(line, '\t');
            if (parts.empty()) {
                continue;
            }
            if (parts[0] == "TABLE") {
                if (parts.size() != 2) {
                    throw std::runtime_error("Corrupt schema file: " + schema_path.string());
                }
                table.name = hexDecode(parts[1]);
                saw_table = true;
                continue;
            }
            if (parts[0] == "COLUMN") {
                if (parts.size() != 5) {
                    throw std::runtime_error("Corrupt schema file: " + schema_path.string());
                }
                ColumnSchema column;
                column.name = hexDecode(parts[1]);
                column.type = parseColumnType(parts[2]);
                column.primary_key = parts[3] == "1";
                column.not_null = parts[4] == "1";
                if (column.primary_key) {
                    table.primary_key_column = table.columns.size();
                }
                table.column_lookup.emplace(normalizeIdentifier(column.name), table.columns.size());
                table.columns.push_back(column);
                continue;
            }
            throw std::runtime_error("Corrupt schema file: " + schema_path.string());
        }

        if (!saw_table || table.columns.empty()) {
            throw std::runtime_error("Incomplete schema file: " + schema_path.string());
        }

        table.cache_namespace = table.rows_path.string();
        initializeRowsFile(table.rows_path, table.row_format);
        table.row_format = hasBinaryRowsMagic(table.rows_path)
            ? RowStorageFormat::BinaryV1
            : RowStorageFormat::LegacyText;
        openTableFiles(table);
        loadRowRefs(table);
        return table;
    }

    void initializeRowsFile(const fs::path &path, RowStorageFormat preferred_format) {
        std::error_code error;
        if (!fs::exists(path, error)) {
            if (preferred_format == RowStorageFormat::BinaryV1) {
                writeFileAtomically(path, std::string(kBinaryRowsMagic, kBinaryRowsMagicSize));
            } else {
                touchFile(path);
            }
            syncDirectory(path.parent_path());
            return;
        }

        if (preferred_format == RowStorageFormat::BinaryV1 && fs::file_size(path, error) == 0) {
            writeFileAtomically(path, std::string(kBinaryRowsMagic, kBinaryRowsMagicSize));
        }
    }

    void loadRowRefs(Table &table) {
        if (table.row_format == RowStorageFormat::BinaryV1) {
            loadBinaryRowRefs(table);
            return;
        }
        loadLegacyTextRowRefs(table);
    }

    void addLoadedRowRef(Table &table, std::uint64_t offset, const LoadedRow &row) {
        RowRef ref;
        ref.offset = offset;
        ref.expires_at_unix_seconds = row.expires_at.has_value()
            ? timePointToUnixSeconds(*row.expires_at)
            : -1;
        table.rows.push_back(ref);

        if (table.primary_key_column.has_value()) {
            const auto &primary_key_value = row.values[*table.primary_key_column];
            if (!primary_key_value.has_value()) {
                throw std::runtime_error("PRIMARY KEY cannot be NULL in persisted data");
            }
            if (!table.primary_index.insert(*primary_key_value, table.rows.size() - 1)) {
                throw std::runtime_error("Duplicate PRIMARY KEY in persisted data");
            }
        }
    }

    void loadLegacyTextRowRefs(Table &table) {
        std::ifstream input(table.rows_path);
        if (!input) {
            throw std::runtime_error("Failed to open rows file: " + table.rows_path.string());
        }

        std::string line;
        std::uint64_t offset = 0;
        while (true) {
            const std::streampos line_start = input.tellg();
            if (!std::getline(input, line)) {
                break;
            }

            try {
                const LoadedRow row = deserializeRowLine(line, table.columns.size());
                addLoadedRowRef(
                    table,
                    line_start != std::streampos(-1) ? static_cast<std::uint64_t>(line_start) : offset,
                    row
                );
            } catch (const std::exception &) {
                if (input.eof()) {
                    break;
                }
                throw;
            }

            offset += static_cast<std::uint64_t>(line.size() + 1);
        }
    }

    void loadBinaryRowRefs(Table &table) {
        std::ifstream input(table.rows_path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Failed to open rows file: " + table.rows_path.string());
        }

        input.seekg(static_cast<std::streamoff>(kBinaryRowsMagicSize));
        std::uint64_t offset = kBinaryRowsMagicSize;

        while (true) {
            char header[4];
            input.read(header, sizeof(header));
            if (input.gcount() == 0) {
                break;
            }
            if (input.gcount() != static_cast<std::streamsize>(sizeof(header))) {
                break;
            }

            std::size_t header_offset = 0;
            std::uint32_t payload_size = 0;
            if (!decodeUint32LE(header, sizeof(header), header_offset, payload_size)) {
                break;
            }

            std::string payload(payload_size, '\0');
            input.read(payload.data(), static_cast<std::streamsize>(payload_size));
            if (input.gcount() != static_cast<std::streamsize>(payload_size)) {
                break;
            }

            const LoadedRow row =
                deserializeBinaryRowRecord(payload.data(), payload.size(), table.columns.size());
            addLoadedRowRef(table, offset, row);
            offset += sizeof(header) + payload_size;
        }
    }

    std::string currentDatabaseKey(const SessionContext &session) const {
        if (session.current_database.empty()) {
            return normalizeIdentifier(kDefaultDatabaseName);
        }
        return normalizeIdentifier(session.current_database);
    }

    DatabaseState &currentDatabaseOrThrow(const SessionContext &session) {
        return getDatabaseOrThrow(currentDatabaseKey(session));
    }

    const DatabaseState &currentDatabaseOrThrow(const SessionContext &session) const {
        return getDatabaseOrThrow(currentDatabaseKey(session));
    }

    DatabaseState &getDatabaseOrThrow(const std::string &name_key) {
        auto it = databases_.find(name_key);
        if (it == databases_.end()) {
            throw std::runtime_error("Unknown database: " + name_key);
        }
        return it->second;
    }

    const DatabaseState &getDatabaseOrThrow(const std::string &name_key) const {
        auto it = databases_.find(name_key);
        if (it == databases_.end()) {
            throw std::runtime_error("Unknown database: " + name_key);
        }
        return it->second;
    }

    void executeCreateDatabase(const CreateDatabaseStatement &statement) {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        const std::string database_key = normalizeIdentifier(statement.database_name);
        if (databases_.find(database_key) != databases_.end()) {
            if (statement.if_not_exists) {
                return;
            }
            throw std::runtime_error("Database already exists: " + statement.database_name);
        }

        DatabaseState database;
        database.name = statement.database_name;
        database.path = data_root_ / database_key;
        createDatabaseOnDisk(database.name, database.path);
        databases_.emplace(database_key, std::move(database));
        cache_.clear();
    }

    void createDatabaseOnDisk(const std::string &name, const fs::path &path) {
        (void)name;
        ensureDirectory(path);
        syncDirectory(path.parent_path());
    }

    void executeUse(SessionContext &session, const UseStatement &statement) {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        const std::string database_key = normalizeIdentifier(statement.database_name);
        getDatabaseOrThrow(database_key);
        session.current_database = statement.database_name;
    }

    void executeCreateTable(SessionContext &session, const CreateTableStatement &statement) {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        DatabaseState &database = currentDatabaseOrThrow(session);
        const std::string table_key = normalizeIdentifier(statement.table_name);
        if (database.tables.find(table_key) != database.tables.end()) {
            if (statement.if_not_exists) {
                return;
            }
            throw std::runtime_error("Table already exists: " + statement.table_name);
        }
        if (statement.columns.empty()) {
            throw std::runtime_error("CREATE TABLE requires at least one column");
        }

        Table table;
        table.name = statement.table_name;
        table.columns = statement.columns;
        table.schema_path = database.path / (table_key + kSchemaExtension);
        table.rows_path = database.path / (table_key + kRowsExtension);
        table.cache_namespace = table.rows_path.string();
        table.row_format = RowStorageFormat::BinaryV1;

        for (std::size_t i = 0; i < table.columns.size(); ++i) {
            const std::string key = normalizeIdentifier(table.columns[i].name);
            if (!table.column_lookup.emplace(key, i).second) {
                throw std::runtime_error("Duplicate column name: " + table.columns[i].name);
            }
            if (table.columns[i].primary_key) {
                table.primary_key_column = i;
            }
        }

        persistTableDefinition(table);
        openTableFiles(table);
        database.tables.emplace(table_key, std::move(table));
        cache_.clear();
    }

    void persistTableDefinition(const Table &table) {
        writeFileAtomically(table.schema_path, serializeSchema(table));
        initializeRowsFile(table.rows_path, table.row_format);
        syncDirectory(table.schema_path.parent_path());
    }

    void executeInsert(SessionContext &session, const InsertStatement &statement) {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        DatabaseState &database = currentDatabaseOrThrow(session);
        Table &table = getTableOrThrow(database, statement.table_name);
        if (statement.rows.empty()) {
            throw std::runtime_error("INSERT requires at least one row");
        }

        std::vector<LoadedRow> normalized_rows;
        normalized_rows.reserve(statement.rows.size());

        for (const auto &incoming_row : statement.rows) {
            if (incoming_row.size() != table.columns.size()) {
                throw std::runtime_error("INSERT value count does not match table schema");
            }

            LoadedRow row;
            row.expires_at = statement.expires_at;
            row.values.reserve(table.columns.size());
            for (std::size_t i = 0; i < table.columns.size(); ++i) {
                row.values.push_back(normalizeLiteralForType(
                    table.columns[i].type,
                    incoming_row[i],
                    table.columns[i].not_null
                ));
            }
            normalized_rows.push_back(std::move(row));
        }

        validatePrimaryKeys(table, normalized_rows);
        appendRows(table, normalized_rows);
        cache_.clear();
    }

    void validatePrimaryKeys(const Table &table, const std::vector<LoadedRow> &rows) const {
        if (!table.primary_key_column.has_value()) {
            return;
        }

        std::unordered_map<std::string, bool> seen_in_batch;
        for (const LoadedRow &row : rows) {
            const auto &primary_key_value = row.values[*table.primary_key_column];
            if (!primary_key_value.has_value()) {
                throw std::runtime_error("PRIMARY KEY cannot be NULL");
            }
            if (table.primary_index.find(*primary_key_value).has_value() ||
                !seen_in_batch.emplace(*primary_key_value, true).second) {
                throw std::runtime_error("Duplicate PRIMARY KEY value");
            }
        }
    }

    void appendRows(Table &table, const std::vector<LoadedRow> &rows) {
        std::string buffer;
        buffer.reserve(rows.size() * std::max<std::size_t>(48, table.columns.size() * 16));
        std::vector<RowRef> refs;
        refs.reserve(rows.size());

        const std::uint64_t base_offset = table.next_offset;
        for (const LoadedRow &row : rows) {
            RowRef ref;
            ref.offset = base_offset + buffer.size();
            ref.expires_at_unix_seconds = row.expires_at.has_value()
                ? timePointToUnixSeconds(*row.expires_at)
                : -1;
            refs.push_back(ref);
            if (table.row_format == RowStorageFormat::BinaryV1) {
                buffer += serializeBinaryRowRecord(row);
            } else {
                buffer += serializeRowLine(row);
            }
        }

        if (table.append_fd < 0) {
            throw std::runtime_error("Table append file is not open");
        }
        writeAll(table.append_fd, buffer);
        durableFileSync(table.append_fd);
        table.next_offset += static_cast<std::uint64_t>(buffer.size());

        for (std::size_t i = 0; i < rows.size(); ++i) {
            table.rows.push_back(refs[i]);
            if (table.primary_key_column.has_value()) {
                const std::string &primary_key_value = *rows[i].values[*table.primary_key_column];
                table.primary_index.insert(primary_key_value, table.rows.size() - 1);
            }
        }
    }

    Table &getTableOrThrow(DatabaseState &database, const std::string &name) {
        const std::string key = normalizeIdentifier(name);
        auto it = database.tables.find(key);
        if (it == database.tables.end()) {
            throw std::runtime_error("Unknown table: " + name);
        }
        return it->second;
    }

    const Table &getTableOrThrow(const DatabaseState &database, const std::string &name) const {
        const std::string key = normalizeIdentifier(name);
        auto it = database.tables.find(key);
        if (it == database.tables.end()) {
            throw std::runtime_error("Unknown table: " + name);
        }
        return it->second;
    }

    LoadedRow readRow(const Table &table, const RowRef &ref) const {
        LoadedRow cached_row;
        if (row_cache_.get(makeRowCacheKey(table, ref.offset), cached_row)) {
            return cached_row;
        }

        if (table.read_fd < 0) {
            throw std::runtime_error("Table read file is not open");
        }

        LoadedRow row;
        if (table.row_format == RowStorageFormat::BinaryV1) {
            row = readBinaryRow(table, ref.offset);
        } else {
            row = readLegacyTextRow(table, ref.offset);
        }
        row_cache_.put(makeRowCacheKey(table, ref.offset), row);
        return row;
    }

    LoadedRow readBinaryRow(const Table &table, std::uint64_t offset) const {
        char header[4];
        while (true) {
            const ssize_t bytes_read =
                pread(table.read_fd, header, sizeof(header), static_cast<off_t>(offset));
            if (bytes_read < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("Failed to read binary row header");
            }
            if (bytes_read != static_cast<ssize_t>(sizeof(header))) {
                throw std::runtime_error("Failed to read binary row header");
            }
            break;
        }

        std::size_t header_offset = 0;
        std::uint32_t payload_size = 0;
        if (!decodeUint32LE(header, sizeof(header), header_offset, payload_size)) {
            throw std::runtime_error("Corrupt binary row header");
        }

        std::string payload(payload_size, '\0');
        std::size_t total_read = 0;
        while (total_read < payload_size) {
            const ssize_t bytes_read = pread(
                table.read_fd,
                payload.data() + total_read,
                payload_size - total_read,
                static_cast<off_t>(offset + sizeof(header) + total_read)
            );
            if (bytes_read < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("Failed to read binary row payload");
            }
            if (bytes_read == 0) {
                throw std::runtime_error("Failed to read binary row payload");
            }
            total_read += static_cast<std::size_t>(bytes_read);
        }

        return deserializeBinaryRowRecord(payload.data(), payload.size(), table.columns.size());
    }

    LoadedRow readLegacyTextRow(const Table &table, std::uint64_t offset) const {
        std::string line;
        line.reserve(256);
        std::uint64_t current_offset = offset;
        char buffer[4096];

        while (true) {
            const ssize_t bytes_read =
                pread(table.read_fd, buffer, sizeof(buffer), static_cast<off_t>(current_offset));
            if (bytes_read < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("Failed to read row from persistent storage");
            }
            if (bytes_read == 0) {
                break;
            }

            const void *newline_ptr = std::memchr(buffer, '\n', static_cast<std::size_t>(bytes_read));
            if (newline_ptr) {
                const char *newline = static_cast<const char *>(newline_ptr);
                line.append(buffer, static_cast<std::size_t>(newline - buffer));
                break;
            }

            line.append(buffer, static_cast<std::size_t>(bytes_read));
            current_offset += static_cast<std::uint64_t>(bytes_read);
        }

        if (line.empty()) {
            throw std::runtime_error("Failed to read row from persistent storage");
        }

        return deserializeRowLine(line, table.columns.size());
    }

    void openTableFiles(Table &table) {
        closeTableFiles(table);

        table.append_fd = open(table.rows_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (table.append_fd < 0) {
            throw std::runtime_error("Failed to open append file: " + table.rows_path.string());
        }

        table.read_fd = open(table.rows_path.c_str(), O_RDONLY);
        if (table.read_fd < 0) {
            close(table.append_fd);
            table.append_fd = -1;
            throw std::runtime_error("Failed to open read file: " + table.rows_path.string());
        }

        table.next_offset = fs::exists(table.rows_path)
            ? static_cast<std::uint64_t>(fs::file_size(table.rows_path))
            : 0;
    }

    void closeTableFiles(Table &table) {
        if (table.append_fd >= 0) {
            close(table.append_fd);
            table.append_fd = -1;
        }
        if (table.read_fd >= 0) {
            close(table.read_fd);
            table.read_fd = -1;
        }
        table.next_offset = 0;
    }

    void closeAllTableFiles() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        closeAllTableFilesUnlocked();
    }

    void closeAllTableFilesUnlocked() {
        for (auto &database_entry : databases_) {
            for (auto &table_entry : database_entry.second.tables) {
                closeTableFiles(table_entry.second);
            }
        }
    }

    void executeSelect(const SessionContext &session, const SelectStatement &statement, QueryResult &result) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        const DatabaseState &database = currentDatabaseOrThrow(session);
        const Table &left_table = getTableOrThrow(database, statement.from_table);
        const Table *right_table = nullptr;
        if (statement.join.has_value()) {
            right_table = &getTableOrThrow(database, statement.join->table_name);
        }

        const std::vector<ResolvedColumn> projection =
            buildProjection(statement, left_table, right_table);

        for (const auto &column : projection) {
            result.columns.push_back(column.output_name);
        }

        if (!statement.join.has_value()) {
            const std::vector<const RowRef *> candidate_rows =
                gatherRowsForSingleTable(statement, left_table);
            for (const RowRef *ref : candidate_rows) {
                if (!ref || isExpired(ref->expires_at_unix_seconds)) {
                    continue;
                }
                const LoadedRow row = readRow(left_table, *ref);
                if (statement.where.has_value() &&
                    !matchesCondition(*statement.where, left_table, row, nullptr, nullptr)) {
                    continue;
                }
                result.rows.push_back(materializeRow(projection, row, nullptr));
            }
            return;
        }

        const JoinClause &join = *statement.join;
        const ResolvedColumn left_join_column =
            resolveColumn(join.left, left_table, right_table, true);
        const ResolvedColumn right_join_column =
            resolveColumn(join.right, left_table, right_table, true);

        if (left_join_column.table == right_join_column.table) {
            throw std::runtime_error("INNER JOIN must compare columns from different tables");
        }

        std::vector<LoadedRow> right_rows;
        right_rows.reserve(right_table->rows.size());
        for (const RowRef &ref : right_table->rows) {
            if (isExpired(ref.expires_at_unix_seconds)) {
                continue;
            }
            right_rows.push_back(readRow(*right_table, ref));
        }

        for (const RowRef &left_ref : left_table.rows) {
            if (isExpired(left_ref.expires_at_unix_seconds)) {
                continue;
            }

            const LoadedRow left_row = readRow(left_table, left_ref);
            for (const LoadedRow &right_row : right_rows) {
                const std::optional<std::string> &left_value =
                    left_join_column.from_right ? right_row.values[left_join_column.index]
                                                : left_row.values[left_join_column.index];
                const std::optional<std::string> &right_value =
                    right_join_column.from_right ? right_row.values[right_join_column.index]
                                                 : left_row.values[right_join_column.index];

                if (!applyComparison(left_join_column.schema->type, left_value, right_value, "=")) {
                    continue;
                }

                if (statement.where.has_value() &&
                    !matchesCondition(*statement.where, left_table, left_row, right_table, &right_row)) {
                    continue;
                }

                result.rows.push_back(materializeRow(projection, left_row, &right_row));
            }
        }
    }

    std::vector<const RowRef *> gatherRowsForSingleTable(
        const SelectStatement &statement,
        const Table &table
    ) const {
        if (statement.where.has_value()) {
            const Condition &condition = *statement.where;
            if (!condition.right.is_identifier &&
                !condition.right.is_null &&
                condition.op == "=" &&
                !condition.left.qualified) {
                const auto column_it = table.column_lookup.find(normalizeIdentifier(condition.left.column));
                if (column_it != table.column_lookup.end() &&
                    table.primary_key_column.has_value() &&
                    column_it->second == *table.primary_key_column) {
                    const Literal primary_key_literal{false, condition.right.text};
                    const auto normalized_key = normalizeLiteralForType(
                        table.columns[*table.primary_key_column].type,
                        primary_key_literal,
                        true
                    );
                    std::vector<const RowRef *> rows;
                    const auto row_index = table.primary_index.find(*normalized_key);
                    if (row_index.has_value()) {
                        rows.push_back(&table.rows[*row_index]);
                    }
                    return rows;
                }
            }
        }

        std::vector<const RowRef *> rows;
        rows.reserve(table.rows.size());
        for (const RowRef &row : table.rows) {
            rows.push_back(&row);
        }
        return rows;
    }

    ResolvedColumn resolveColumn(
        const ColumnRef &ref,
        const Table &left_table,
        const Table *right_table,
        bool qualify_join_projection
    ) const {
        auto resolve_in_table =
            [&](const Table &table, bool from_right, const std::string &output_prefix) -> ResolvedColumn {
            const auto it = table.column_lookup.find(normalizeIdentifier(ref.column));
            if (it == table.column_lookup.end()) {
                return {};
            }
            const ColumnSchema &schema = table.columns[it->second];
            ResolvedColumn resolved;
            resolved.table = &table;
            resolved.schema = &schema;
            resolved.index = it->second;
            resolved.from_right = from_right;
            resolved.output_name =
                output_prefix.empty() ? schema.name : output_prefix + "." + schema.name;
            return resolved;
        };

        if (ref.qualified) {
            const std::string table_name = normalizeIdentifier(ref.table);
            if (table_name == normalizeIdentifier(left_table.name)) {
                ResolvedColumn resolved =
                    resolve_in_table(left_table, false, qualify_join_projection ? left_table.name : "");
                if (!resolved.table) {
                    throw std::runtime_error("Unknown column: " + ref.raw);
                }
                resolved.output_name = ref.raw;
                return resolved;
            }
            if (right_table && table_name == normalizeIdentifier(right_table->name)) {
                ResolvedColumn resolved =
                    resolve_in_table(*right_table, true, qualify_join_projection ? right_table->name : "");
                if (!resolved.table) {
                    throw std::runtime_error("Unknown column: " + ref.raw);
                }
                resolved.output_name = ref.raw;
                return resolved;
            }
            throw std::runtime_error("Unknown table in column reference: " + ref.raw);
        }

        ResolvedColumn resolved_left = resolve_in_table(left_table, false, "");
        ResolvedColumn resolved_right;
        if (right_table) {
            resolved_right = resolve_in_table(*right_table, true, "");
        }

        if (resolved_left.table && resolved_right.table) {
            throw std::runtime_error("Ambiguous column reference: " + ref.column);
        }
        if (resolved_left.table) {
            if (right_table && qualify_join_projection) {
                resolved_left.output_name = left_table.name + "." + resolved_left.schema->name;
            }
            return resolved_left;
        }
        if (resolved_right.table) {
            if (qualify_join_projection) {
                resolved_right.output_name = right_table->name + "." + resolved_right.schema->name;
            }
            return resolved_right;
        }

        throw std::runtime_error("Unknown column: " + ref.column);
    }

    std::vector<ResolvedColumn> buildProjection(
        const SelectStatement &statement,
        const Table &left_table,
        const Table *right_table
    ) const {
        std::vector<ResolvedColumn> projection;

        if (statement.select_all) {
            for (std::size_t i = 0; i < left_table.columns.size(); ++i) {
                ResolvedColumn column;
                column.table = &left_table;
                column.schema = &left_table.columns[i];
                column.index = i;
                column.from_right = false;
                column.output_name = right_table
                    ? left_table.name + "." + left_table.columns[i].name
                    : left_table.columns[i].name;
                projection.push_back(column);
            }

            if (right_table) {
                for (std::size_t i = 0; i < right_table->columns.size(); ++i) {
                    ResolvedColumn column;
                    column.table = right_table;
                    column.schema = &right_table->columns[i];
                    column.index = i;
                    column.from_right = true;
                    column.output_name = right_table->name + "." + right_table->columns[i].name;
                    projection.push_back(column);
                }
            }

            return projection;
        }

        projection.reserve(statement.columns.size());
        for (const ColumnRef &ref : statement.columns) {
            projection.push_back(resolveColumn(ref, left_table, right_table, false));
        }
        return projection;
    }

    std::vector<std::optional<std::string>> materializeRow(
        const std::vector<ResolvedColumn> &projection,
        const LoadedRow &left_row,
        const LoadedRow *right_row
    ) const {
        std::vector<std::optional<std::string>> values;
        values.reserve(projection.size());

        for (const ResolvedColumn &column : projection) {
            if (column.from_right) {
                values.push_back(right_row ? right_row->values[column.index] : std::nullopt);
            } else {
                values.push_back(left_row.values[column.index]);
            }
        }

        return values;
    }

    bool matchesCondition(
        const Condition &condition,
        const Table &left_table,
        const LoadedRow &left_row,
        const Table *right_table,
        const LoadedRow *right_row
    ) const {
        const ResolvedColumn left = resolveColumn(condition.left, left_table, right_table, false);
        const std::optional<std::string> &left_value =
            left.from_right ? right_row->values[left.index] : left_row.values[left.index];

        std::optional<std::string> right_value;
        ColumnType comparison_type = left.schema->type;

        if (condition.right.is_identifier) {
            if (!right_table || !right_row) {
                throw std::runtime_error("Identifier comparison requires joined tables");
            }
            const ResolvedColumn right =
                resolveColumn(condition.right.identifier, left_table, right_table, false);
            right_value = right.from_right ? right_row->values[right.index] : left_row.values[right.index];
        } else if (condition.right.is_null) {
            right_value = std::nullopt;
        } else {
            const Literal literal{false, condition.right.text};
            right_value = normalizeLiteralForType(comparison_type, literal, false);
        }

        return applyComparison(comparison_type, left_value, right_value, condition.op);
    }

    mutable std::shared_mutex mutex_;
    fs::path data_root_;
    std::unordered_map<std::string, DatabaseState> databases_;
    QueryCache cache_{128};
    mutable RowCache row_cache_{16384};
};

Database::Database() : impl_(std::make_unique<Impl>()) {}

Database::~Database() = default;

bool Database::execute(SessionContext &session, const std::string &sql, QueryResult &result, std::string &error) {
    return impl_->execute(session, sql, result, error);
}

bool Database::execute(const std::string &sql, QueryResult &result, std::string &error) {
    SessionContext session;
    return impl_->execute(session, sql, result, error);
}

}  // namespace flexql
