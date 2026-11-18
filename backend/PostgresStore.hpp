#ifndef SKYWARD_POSTGRES_STORE_HPP
#define SKYWARD_POSTGRES_STORE_HPP

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class PostgresStore {
public:
    explicit PostgresStore(const std::string& connectionString);
    ~PostgresStore();

    PostgresStore(const PostgresStore&) = delete;
    PostgresStore& operator=(const PostgresStore&) = delete;

    void initializeAndImport(const std::filesystem::path& dataDirectory,
                             const std::filesystem::path& schemaFile);
    std::vector<std::string> loadRecords(const std::string& recordType) const;
    void replaceRecords(const std::string& recordType,
                        const std::vector<std::string>& records);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
