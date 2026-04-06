#include "flexql.h"

#include "database.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct FlexQL {
    int socket_fd = -1;
};

namespace {

bool sendAll(int fd, const void *buffer, std::size_t length) {
    const auto *bytes = static_cast<const unsigned char *>(buffer);
    std::size_t sent = 0;

    while (sent < length) {
        const ssize_t rc = send(fd, bytes + sent, length - sent, 0);
        if (rc <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(rc);
    }

    return true;
}

bool recvAll(int fd, void *buffer, std::size_t length) {
    auto *bytes = static_cast<unsigned char *>(buffer);
    std::size_t received = 0;

    while (received < length) {
        const ssize_t rc = recv(fd, bytes + received, length - received, 0);
        if (rc <= 0) {
            return false;
        }
        received += static_cast<std::size_t>(rc);
    }

    return true;
}

bool sendUint32(int fd, std::uint32_t value) {
    const std::uint32_t network = htonl(value);
    return sendAll(fd, &network, sizeof(network));
}

bool recvUint32(int fd, std::uint32_t &value) {
    std::uint32_t network = 0;
    if (!recvAll(fd, &network, sizeof(network))) {
        return false;
    }
    value = ntohl(network);
    return true;
}

bool recvByte(int fd, unsigned char &value) {
    return recvAll(fd, &value, sizeof(value));
}

bool sendString(int fd, const std::string &value) {
    if (!sendUint32(fd, static_cast<std::uint32_t>(value.size()))) {
        return false;
    }
    return value.empty() ? true : sendAll(fd, value.data(), value.size());
}

bool recvString(int fd, std::string &value) {
    std::uint32_t size = 0;
    if (!recvUint32(fd, size)) {
        return false;
    }

    value.assign(size, '\0');
    return size == 0 ? true : recvAll(fd, value.data(), size);
}

bool recvOptionalString(int fd, std::optional<std::string> &value) {
    unsigned char present = 0;
    if (!recvByte(fd, present)) {
        return false;
    }
    if (present == 0) {
        value = std::nullopt;
        return true;
    }
    std::string text;
    if (!recvString(fd, text)) {
        return false;
    }
    value = text;
    return true;
}

char *copyCString(const std::string &value) {
    char *buffer = static_cast<char *>(std::malloc(value.size() + 1));
    if (!buffer) {
        return nullptr;
    }
    std::memcpy(buffer, value.c_str(), value.size() + 1);
    return buffer;
}

void setErrorMessage(char **errmsg, const std::string &message) {
    if (!errmsg) {
        return;
    }
    *errmsg = copyCString(message);
}

}  // namespace

int flexql_open(const char *host, int port, FlexQL **db) {
    if (!host || !db) {
        return FLEXQL_ERROR;
    }

    *db = nullptr;

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *result = nullptr;
    const std::string port_text = std::to_string(port);
    if (getaddrinfo(host, port_text.c_str(), &hints, &result) != 0) {
        return FLEXQL_ERROR;
    }

    int socket_fd = -1;
    for (struct addrinfo *it = result; it != nullptr; it = it->ai_next) {
        socket_fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (socket_fd < 0) {
            continue;
        }
        if (connect(socket_fd, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        close(socket_fd);
        socket_fd = -1;
    }

    freeaddrinfo(result);

    if (socket_fd < 0) {
        return FLEXQL_ERROR;
    }

    FlexQL *handle = new FlexQL();
    handle->socket_fd = socket_fd;
    *db = handle;
    return FLEXQL_OK;
}

int flexql_close(FlexQL *db) {
    if (!db) {
        return FLEXQL_ERROR;
    }

    if (db->socket_fd >= 0) {
        close(db->socket_fd);
        db->socket_fd = -1;
    }

    delete db;
    return FLEXQL_OK;
}

int flexql_exec(
    FlexQL *db,
    const char *sql,
    int (*callback)(void *, int, char **, char **),
    void *arg,
    char **errmsg
) {
    if (errmsg) {
        *errmsg = nullptr;
    }

    if (!db || !sql) {
        setErrorMessage(errmsg, "Invalid database handle or SQL input");
        return FLEXQL_ERROR;
    }

    const std::string query(sql);
    if (!sendString(db->socket_fd, query)) {
        setErrorMessage(errmsg, "Failed to send SQL statement to server");
        return FLEXQL_ERROR;
    }

    std::uint32_t status = 0;
    if (!recvUint32(db->socket_fd, status)) {
        setErrorMessage(errmsg, "Failed to read server response");
        return FLEXQL_ERROR;
    }

    if (status != FLEXQL_OK) {
        std::string error;
        if (!recvString(db->socket_fd, error)) {
            setErrorMessage(errmsg, "Failed to read error message from server");
            return FLEXQL_ERROR;
        }
        setErrorMessage(errmsg, error);
        return FLEXQL_ERROR;
    }

    std::uint32_t column_count = 0;
    std::uint32_t row_count = 0;
    if (!recvUint32(db->socket_fd, column_count) || !recvUint32(db->socket_fd, row_count)) {
        setErrorMessage(errmsg, "Malformed server response");
        return FLEXQL_ERROR;
    }

    std::vector<std::string> column_names(column_count);
    for (std::uint32_t i = 0; i < column_count; ++i) {
        if (!recvString(db->socket_fd, column_names[i])) {
            setErrorMessage(errmsg, "Failed to read column names from server");
            return FLEXQL_ERROR;
        }
    }

    std::vector<char *> callback_column_names(column_count);
    for (std::uint32_t i = 0; i < column_count; ++i) {
        callback_column_names[i] = column_count == 0
            ? nullptr
            : const_cast<char *>(column_names[i].c_str());
    }

    for (std::uint32_t row_index = 0; row_index < row_count; ++row_index) {
        std::vector<std::optional<std::string>> row(column_count);
        for (std::uint32_t column_index = 0; column_index < column_count; ++column_index) {
            if (!recvOptionalString(db->socket_fd, row[column_index])) {
                setErrorMessage(errmsg, "Failed to read row data from server");
                return FLEXQL_ERROR;
            }
        }

        if (callback) {
            std::vector<char *> values(column_count, nullptr);
            for (std::uint32_t column_index = 0; column_index < column_count; ++column_index) {
                values[column_index] = row[column_index].has_value()
                    ? const_cast<char *>(row[column_index]->c_str())
                    : nullptr;
            }
            const int rc = callback(
                arg,
                static_cast<int>(column_count),
                values.data(),
                callback_column_names.data()
            );
            if (rc != 0) {
                break;
            }
        }
    }

    return FLEXQL_OK;
}

void flexql_free(void *ptr) {
    std::free(ptr);
}
