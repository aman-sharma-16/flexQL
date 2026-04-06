#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>

namespace flexql {

class PrimaryIndex {
public:
    bool insert(const std::string &key, std::size_t row_index) {
        return rows_.emplace(key, row_index).second;
    }

    std::optional<std::size_t> find(const std::string &key) const {
        auto it = rows_.find(key);
        if (it == rows_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void clear() {
        rows_.clear();
    }

private:
    std::unordered_map<std::string, std::size_t> rows_;
};

}  // namespace flexql
