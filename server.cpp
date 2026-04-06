#include "database.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

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

bool sendByte(int fd, unsigned char value) {
    return sendAll(fd, &value, sizeof(value));
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

bool sendOptionalString(int fd, const std::optional<std::string> &value) {
    if (!sendByte(fd, value.has_value() ? 1U : 0U)) {
        return false;
    }
    if (!value.has_value()) {
        return true;
    }
    return sendString(fd, *value);
}

class Server {
public:
    explicit Server(int port) : port_(port) {}

    bool run() {
        const int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            std::cerr << "socket() failed\n";
            return false;
        }

        const int reuse = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address;
        std::memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(static_cast<std::uint16_t>(port_));

        if (bind(server_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
            std::cerr << "bind() failed\n";
            close(server_fd);
            return false;
        }

        if (listen(server_fd, 32) < 0) {
            std::cerr << "listen() failed\n";
            close(server_fd);
            return false;
        }

        std::cout << "FlexQL server listening on port " << port_ << '\n';

        while (true) {
            sockaddr_in client_address;
            socklen_t client_length = sizeof(client_address);
            const int client_fd =
                accept(server_fd, reinterpret_cast<sockaddr *>(&client_address), &client_length);

            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::cerr << "accept() failed\n";
                continue;
            }

            std::thread(&Server::handleClient, this, client_fd).detach();
        }
    }

private:
    void handleClient(int client_fd) {
        flexql::SessionContext session;
        while (true) {
            std::string sql;
            if (!recvString(client_fd, sql)) {
                break;
            }

            flexql::QueryResult result;
            std::string error;
            const bool ok = db_.execute(session, sql, result, error);

            if (!sendUint32(client_fd, ok ? 0U : 1U)) {
                break;
            }

            if (!ok) {
                if (!sendString(client_fd, error)) {
                    break;
                }
                continue;
            }

            if (!sendUint32(client_fd, static_cast<std::uint32_t>(result.columns.size())) ||
                !sendUint32(client_fd, static_cast<std::uint32_t>(result.rows.size()))) {
                break;
            }

            for (const std::string &column : result.columns) {
                if (!sendString(client_fd, column)) {
                    close(client_fd);
                    return;
                }
            }

            for (const auto &row : result.rows) {
                for (const auto &value : row) {
                    if (!sendOptionalString(client_fd, value)) {
                        close(client_fd);
                        return;
                    }
                }
            }
        }

        close(client_fd);
    }

    int port_;
    flexql::Database db_;
};

}  // namespace

int main(int argc, char **argv) {
    int port = 9000;
    if (argc >= 2) {
        port = std::stoi(argv[1]);
    }

    Server server(port);
    return server.run() ? 0 : 1;
}
