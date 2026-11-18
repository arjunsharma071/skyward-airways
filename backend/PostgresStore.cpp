#include "PostgresStore.hpp"

#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <system_error>

#ifdef SKYWARD_WITH_POSTGRES
#include <libpq-fe.h>

#include <vector>
#endif

struct PostgresStore::Impl {
#ifdef SKYWARD_WITH_POSTGRES
    explicit Impl(const std::string& connectionString)
        : connection(PQconnectdb(connectionString.c_str())) {
        if (connection == nullptr || PQstatus(connection) != CONNECTION_OK) {
            const std::string message = connection == nullptr
                ? "PostgreSQL could not allocate a connection."
                : std::string("PostgreSQL connection failed: ") + PQerrorMessage(connection);
            if (connection != nullptr) {
                PQfinish(connection);
                connection = nullptr;
            }
            throw std::runtime_error(message);
        }
    }

    ~Impl() {
        if (connection != nullptr) PQfinish(connection);
    }

    PGconn* connection = nullptr;
#else
    explicit Impl(const std::string&) {
        throw std::runtime_error(
            "This build does not include PostgreSQL support; rebuild with SKYWARD_WITH_POSTGRES.");
    }
#endif
};

#ifdef SKYWARD_WITH_POSTGRES
namespace {
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
constexpr long long POSTGRES_WRITE_LOCK = 782415309LL;

Result execute(PGconn* connection, const std::string& sql) {
    Result result(PQexec(connection, sql.c_str()), &PQclear);
    if (!result || PQresultStatus(result.get()) != PGRES_COMMAND_OK) {
        const char* detail = result ? PQresultErrorMessage(result.get())
                                    : PQerrorMessage(connection);
        throw std::runtime_error(std::string("PostgreSQL operation failed: ") +
                                 (detail == nullptr ? "unknown database error" : detail));
    }
    return result;
}

Result executeParams(PGconn* connection, const std::string& sql,
                     const std::vector<std::string>& parameters,
                     ExecStatusType expectedStatus = PGRES_COMMAND_OK) {
    std::vector<const char*> values;
    values.reserve(parameters.size());
    for (const std::string& parameter : parameters) values.push_back(parameter.c_str());
    Result result(PQexecParams(connection, sql.c_str(),
                               static_cast<int>(values.size()), nullptr,
                               values.empty() ? nullptr : values.data(),
                               nullptr, nullptr, 0),
                  &PQclear);
    if (!result || PQresultStatus(result.get()) != expectedStatus) {
        const char* detail = result ? PQresultErrorMessage(result.get())
                                    : PQerrorMessage(connection);
        throw std::runtime_error(std::string("PostgreSQL operation failed: ") +
                                 (detail == nullptr ? "unknown database error" : detail));
    }
    return result;
}

void beginTransaction(PGconn* connection) {
    execute(connection, "BEGIN");
}

void commitTransaction(PGconn* connection) {
    execute(connection, "COMMIT");
}

void rollbackTransaction(PGconn* connection) noexcept {
    PGresult* result = PQexec(connection, "ROLLBACK");
    if (result != nullptr) PQclear(result);
}

void validateRecordType(const std::string& recordType) {
    if (recordType != "flights" && recordType != "passengers" &&
        recordType != "bookings") {
        throw std::invalid_argument("Unsupported PostgreSQL record type.");
    }
}

std::string recordKey(const std::string& line) {
    const std::size_t separator = line.find('|');
    const std::string key = line.substr(0, separator);
    if (key.empty()) throw std::runtime_error("A legacy data record has an empty key.");
    return key;
}

void lockWrites(PGconn* connection) {
    executeParams(connection, "SELECT pg_advisory_xact_lock($1::bigint)",
                  {std::to_string(POSTGRES_WRITE_LOCK)}, PGRES_TUPLES_OK);
}
}  // namespace
#endif

PostgresStore::PostgresStore(const std::string& connectionString)
    : impl_(std::make_unique<Impl>(connectionString)) {}

PostgresStore::~PostgresStore() = default;

void PostgresStore::initializeAndImport(
    const std::filesystem::path& dataDirectory,
    const std::filesystem::path& schemaFile) {
#ifdef SKYWARD_WITH_POSTGRES
    std::ifstream schemaInput(schemaFile, std::ios::binary);
    if (!schemaInput) {
        throw std::runtime_error("Could not open database schema: " + schemaFile.string());
    }
    std::ostringstream schemaContents;
    schemaContents << schemaInput.rdbuf();
    if (schemaInput.bad()) {
        throw std::runtime_error("Could not read database schema: " + schemaFile.string());
    }
    execute(impl_->connection, schemaContents.str());

    const std::vector<std::pair<std::string, std::string>> sources = {
        {"flights", "flights.txt"},
        {"passengers", "passengers.txt"},
        {"bookings", "bookings.txt"}
    };
    for (const auto& source : sources) {
        const std::filesystem::path path = dataDirectory / source.second;
        std::ifstream input(path);
        if (!input && std::filesystem::exists(path)) {
            throw std::runtime_error("Could not open legacy data file: " + path.string());
        }
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty()) lines.push_back(line);
        }
        if (input.bad()) {
            throw std::runtime_error("Could not read legacy data file: " + path.string());
        }

        beginTransaction(impl_->connection);
        try {
            lockWrites(impl_->connection);
            Result imported = executeParams(
                impl_->connection,
                "SELECT 1 FROM skyward_legacy_imports WHERE source_name = $1",
                {source.second}, PGRES_TUPLES_OK);
            if (PQntuples(imported.get()) == 0) {
                for (std::size_t index = 0; index < lines.size(); ++index) {
                    const std::string key = recordKey(lines[index]);
                    executeParams(
                        impl_->connection,
                        "INSERT INTO skyward_records "
                        "(record_type, record_key, record_data, ordinal) "
                        "VALUES ($1, $2, $3, $4::bigint) ON CONFLICT "
                        "(record_type, record_key) DO NOTHING",
                        {source.first, key, lines[index], std::to_string(index)});
                    Result saved = executeParams(
                        impl_->connection,
                        "SELECT record_data FROM skyward_records "
                        "WHERE record_type = $1 AND record_key = $2",
                        {source.first, key}, PGRES_TUPLES_OK);
                    if (PQntuples(saved.get()) != 1 ||
                        lines[index] != PQgetvalue(saved.get(), 0, 0)) {
                        throw std::runtime_error(
                            "Legacy import conflict for record key " + key +
                            "; the database record was not overwritten.");
                    }
                }
                executeParams(
                    impl_->connection,
                    "INSERT INTO skyward_legacy_imports (source_name, imported_rows) "
                    "VALUES ($1, $2::bigint)",
                    {source.second, std::to_string(lines.size())});
            }
            commitTransaction(impl_->connection);
        } catch (...) {
            rollbackTransaction(impl_->connection);
            throw;
        }
    }
#else
    (void)dataDirectory;
    (void)schemaFile;
    throw std::runtime_error(
        "PostgreSQL support is unavailable in this build; compile with SKYWARD_WITH_POSTGRES.");
#endif
}

std::vector<std::string> PostgresStore::loadRecords(
    const std::string& recordType) const {
#ifdef SKYWARD_WITH_POSTGRES
    validateRecordType(recordType);
    Result result = executeParams(
        impl_->connection,
        "SELECT record_data FROM skyward_records WHERE record_type = $1 "
        "ORDER BY ordinal, record_key",
        {recordType}, PGRES_TUPLES_OK);
    std::vector<std::string> records;
    records.reserve(static_cast<std::size_t>(PQntuples(result.get())));
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        records.emplace_back(PQgetvalue(result.get(), row, 0));
    }
    return records;
#else
    (void)recordType;
    throw std::runtime_error(
        "PostgreSQL support is unavailable in this build; compile with SKYWARD_WITH_POSTGRES.");
#endif
}

void PostgresStore::replaceRecords(const std::string& recordType,
                                   const std::vector<std::string>& records) {
#ifdef SKYWARD_WITH_POSTGRES
    validateRecordType(recordType);
    beginTransaction(impl_->connection);
    try {
        lockWrites(impl_->connection);
        executeParams(impl_->connection,
                      "DELETE FROM skyward_records WHERE record_type = $1",
                      {recordType});
        for (std::size_t index = 0; index < records.size(); ++index) {
            executeParams(
                impl_->connection,
                "INSERT INTO skyward_records "
                "(record_type, record_key, record_data, ordinal) "
                "VALUES ($1, $2, $3, $4::bigint)",
                {recordType, recordKey(records[index]), records[index],
                 std::to_string(index)});
        }
        commitTransaction(impl_->connection);
    } catch (...) {
        rollbackTransaction(impl_->connection);
        throw;
    }
#else
    (void)recordType;
    (void)records;
    throw std::runtime_error(
        "PostgreSQL support is unavailable in this build; compile with SKYWARD_WITH_POSTGRES.");
#endif
}
