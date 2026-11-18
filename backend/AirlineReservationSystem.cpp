#include "AirlineReservationSystem.hpp"
#include "PostgresStore.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <unistd.h>
#endif

namespace {
const char FIELD_SEPARATOR = '|';
const double BUSINESS_MULTIPLIER = 1.8;
const int MAX_PARTY_SIZE = 9;
const char PASSWORD_HASH_PREFIX[] = "pbkdf2-sha256$";
using PasswordIterationCount = unsigned long;
const PasswordIterationCount PASSWORD_HASH_ITERATIONS = 600000;
const PasswordIterationCount MIN_PASSWORD_HASH_ITERATIONS = 100000;
const PasswordIterationCount MAX_PASSWORD_HASH_ITERATIONS = 2000000;
const std::size_t PASSWORD_SALT_BYTES = 16;
const std::size_t PASSWORD_HASH_BYTES = 32;

std::string trim(const std::string& value) {
    const std::string whitespace = " \t\r\n";
    const std::size_t first = value.find_first_not_of(whitespace);
    if (first == std::string::npos) return "";
    return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

void validateText(const std::string& value, const std::string& field,
                  std::size_t maximumLength = 100) {
    if (trim(value).empty()) throw std::invalid_argument(field + " is required.");
    if (value.size() > maximumLength) {
        throw std::invalid_argument(field + " is too long.");
    }
    if (value.find(FIELD_SEPARATOR) != std::string::npos ||
        value.find('\n') != std::string::npos || value.find('\r') != std::string::npos) {
        throw std::invalid_argument(field + " contains an unsupported character.");
    }
}

bool validDate(const std::string& date) {
    static const std::regex pattern(R"(^(\d{4})-(\d{2})-(\d{2})$)");
    std::smatch match;
    if (!std::regex_match(date, match, pattern)) return false;
    const int year = std::stoi(match[1]);
    const int month = std::stoi(match[2]);
    const int day = std::stoi(match[3]);
    if (month < 1 || month > 12) return false;
    const bool leapYear = year % 400 == 0 || (year % 4 == 0 && year % 100 != 0);
    const int monthLengths[] = {31, leapYear ? 29 : 28, 31, 30, 31, 30,
                                31, 31, 30, 31, 30, 31};
    return day >= 1 && day <= monthLengths[month - 1];
}

bool validTime(const std::string& time) {
    static const std::regex pattern(R"(^([01]\d|2[0-3]):[0-5]\d$)");
    return std::regex_match(time, pattern);
}

std::string estimateArrival(const std::string& departure) {
    if (!validTime(departure)) return "00:00";
    const int hour = std::stoi(departure.substr(0, 2));
    const int minute = std::stoi(departure.substr(3, 2));
    const int arrivalMinutes = (hour * 60 + minute + 150) % (24 * 60);
    std::ostringstream result;
    result << std::setfill('0') << std::setw(2) << arrivalMinutes / 60 << ':'
           << std::setw(2) << arrivalMinutes % 60;
    return result.str();
}

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, FIELD_SEPARATOR)) fields.push_back(field);
    if (!line.empty() && line.back() == FIELD_SEPARATOR) fields.emplace_back();
    return fields;
}

std::vector<std::string> splitList(const std::string& value) {
    std::vector<std::string> items;
    if (value.empty()) return items;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) items.push_back(item);
    }
    return items;
}

std::string joinList(const std::vector<std::string>& items) {
    std::ostringstream result;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) result << ',';
        result << items[index];
    }
    return result.str();
}

double parseDouble(const std::string& value) {
    std::size_t consumed = 0;
    const double result = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(result)) {
        throw std::runtime_error("Invalid numeric value in data file.");
    }
    return result;
}

int parseInt(const std::string& value) {
    std::size_t consumed = 0;
    const int result = std::stoi(value, &consumed);
    if (consumed != value.size()) {
        throw std::runtime_error("Invalid integer value in data file.");
    }
    return result;
}

std::string money(double amount) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(2) << amount;
    return output.str();
}

void validateFlight(const Flight& flight) {
    validateText(flight.number, "Flight number", 20);
    validateText(flight.airline, "Airline", 80);
    validateText(flight.source, "Source", 80);
    validateText(flight.destination, "Destination", 80);
    if (lower(flight.source) == lower(flight.destination)) {
        throw std::invalid_argument("Source and destination must be different.");
    }
    if (!validDate(flight.date)) {
        throw std::invalid_argument("Travel date must be a real date in YYYY-MM-DD format.");
    }
    if (!validTime(flight.departureTime) || !validTime(flight.arrivalTime)) {
        throw std::invalid_argument("Flight times must use 24-hour HH:MM format.");
    }
    if (!std::isfinite(flight.baseFare) || flight.baseFare <= 0.0 ||
        flight.baseFare > 100000000.0) {
        throw std::invalid_argument("Base fare must be greater than zero.");
    }
    if (flight.economySeats < 0 || flight.businessSeats < 0 ||
        flight.economySeats > 100000 || flight.businessSeats > 100000) {
        throw std::invalid_argument("Seat counts must be between 0 and 100000.");
    }
}

bool constantTimeEqual(const std::string& left, const std::string& right) {
    std::size_t difference = left.size() ^ right.size();
    const std::size_t length = std::max(left.size(), right.size());
    for (std::size_t index = 0; index < length; ++index) {
        const unsigned char leftByte = index < left.size()
            ? static_cast<unsigned char>(left[index]) : 0;
        const unsigned char rightByte = index < right.size()
            ? static_cast<unsigned char>(right[index]) : 0;
        difference |= static_cast<std::size_t>(leftByte ^ rightByte);
    }
    return difference == 0;
}

#ifdef _WIN32
class Sha256HmacProvider {
public:
    Sha256HmacProvider() {
        const NTSTATUS status = BCryptOpenAlgorithmProvider(
            &handle_, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
        if (status < 0) {
            throw std::runtime_error("Could not initialize the Windows password-hashing provider.");
        }
    }

    ~Sha256HmacProvider() {
        if (handle_ != nullptr) BCryptCloseAlgorithmProvider(handle_, 0);
    }

    Sha256HmacProvider(const Sha256HmacProvider&) = delete;
    Sha256HmacProvider& operator=(const Sha256HmacProvider&) = delete;

    BCRYPT_ALG_HANDLE get() const { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

std::vector<UCHAR> derivePasswordKey(const std::string& password,
                                     const std::vector<UCHAR>& salt,
                                     PasswordIterationCount iterations) {
    Sha256HmacProvider provider;
    std::vector<UCHAR> derivedKey(PASSWORD_HASH_BYTES);
    const NTSTATUS status = BCryptDeriveKeyPBKDF2(
        provider.get(),
        reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
        static_cast<ULONG>(password.size()),
        const_cast<PUCHAR>(salt.data()), static_cast<ULONG>(salt.size()),
        iterations, derivedKey.data(), static_cast<ULONG>(derivedKey.size()), 0);
    if (status < 0) {
        throw std::runtime_error("Windows could not derive the passenger password hash.");
    }
    return derivedKey;
}
#elif defined(__linux__) || defined(__APPLE__)
std::vector<unsigned char> derivePasswordKey(
    const std::string& password, const std::vector<unsigned char>& salt,
    PasswordIterationCount iterations) {
    if (password.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        salt.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Password input is too large to hash.");
    }
    std::vector<unsigned char> derivedKey(PASSWORD_HASH_BYTES);
    const int result = PKCS5_PBKDF2_HMAC(
        password.data(), static_cast<int>(password.size()), salt.data(),
        static_cast<int>(salt.size()), static_cast<int>(iterations), EVP_sha256(),
        static_cast<int>(derivedKey.size()), derivedKey.data());
    if (result != 1) {
        throw std::runtime_error("OpenSSL could not derive the passenger password hash.");
    }
    return derivedKey;
}
#else
std::vector<unsigned char> derivePasswordKey(const std::string&,
                                             const std::vector<unsigned char>&,
                                             unsigned long) {
    throw std::runtime_error("Secure passenger password hashing is unavailable on this platform.");
}
#endif

std::string encodeHex(const unsigned char* bytes, std::size_t length) {
    static const char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(length * 2);
    for (std::size_t index = 0; index < length; ++index) {
        encoded.push_back(digits[bytes[index] >> 4]);
        encoded.push_back(digits[bytes[index] & 0x0F]);
    }
    return encoded;
}

std::vector<unsigned char> decodeHex(const std::string& encoded,
                                     std::size_t expectedBytes) {
    if (encoded.size() != expectedBytes * 2) {
        throw std::runtime_error("Invalid password hash data in passengers file.");
    }
    const auto valueOf = [](char digit) -> int {
        if (digit >= '0' && digit <= '9') return digit - '0';
        if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
        if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
        return -1;
    };
    std::vector<unsigned char> decoded(expectedBytes);
    for (std::size_t index = 0; index < expectedBytes; ++index) {
        const int high = valueOf(encoded[index * 2]);
        const int low = valueOf(encoded[index * 2 + 1]);
        if (high < 0 || low < 0) {
            throw std::runtime_error("Invalid password hash data in passengers file.");
        }
        decoded[index] = static_cast<unsigned char>((high << 4) | low);
    }
    return decoded;
}

struct ParsedPasswordHash {
    PasswordIterationCount iterations;
    std::vector<unsigned char> salt;
    std::vector<unsigned char> hash;
};

bool hasPasswordHashPrefix(const std::string& storedPassword) {
    return storedPassword.compare(0, sizeof(PASSWORD_HASH_PREFIX) - 1,
                                  PASSWORD_HASH_PREFIX) == 0;
}

ParsedPasswordHash parsePasswordHash(const std::string& storedPassword) {
    if (!hasPasswordHashPrefix(storedPassword)) {
        throw std::runtime_error("Passenger password is not in the supported hashed format.");
    }
    const std::string fields = storedPassword.substr(sizeof(PASSWORD_HASH_PREFIX) - 1);
    const std::size_t firstSeparator = fields.find('$');
    const std::size_t secondSeparator = firstSeparator == std::string::npos
        ? std::string::npos : fields.find('$', firstSeparator + 1);
    if (firstSeparator == std::string::npos || secondSeparator == std::string::npos ||
        fields.find('$', secondSeparator + 1) != std::string::npos) {
        throw std::runtime_error("Invalid password hash data in passengers file.");
    }
    const std::string iterationField = fields.substr(0, firstSeparator);
    std::size_t consumed = 0;
    unsigned long parsedIterations = 0;
    try {
        parsedIterations = std::stoul(iterationField, &consumed);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid password hash data in passengers file.");
    }
    if (consumed != iterationField.size() ||
        parsedIterations < MIN_PASSWORD_HASH_ITERATIONS ||
        parsedIterations > MAX_PASSWORD_HASH_ITERATIONS) {
        throw std::runtime_error("Unsupported password hash parameters in passengers file.");
    }
    return ParsedPasswordHash{
        static_cast<PasswordIterationCount>(parsedIterations),
        decodeHex(fields.substr(firstSeparator + 1,
                                 secondSeparator - firstSeparator - 1),
                  PASSWORD_SALT_BYTES),
        decodeHex(fields.substr(secondSeparator + 1), PASSWORD_HASH_BYTES)};
}

std::string hashPassword(const std::string& password) {
#ifdef _WIN32
    std::array<UCHAR, PASSWORD_SALT_BYTES> salt{};
    const NTSTATUS status = BCryptGenRandom(
        nullptr, salt.data(), static_cast<ULONG>(salt.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        throw std::runtime_error("Windows could not generate a secure password salt.");
    }
    const std::vector<UCHAR> saltVector(salt.begin(), salt.end());
    const std::vector<UCHAR> derivedKey =
        derivePasswordKey(password, saltVector, PASSWORD_HASH_ITERATIONS);
    return std::string(PASSWORD_HASH_PREFIX) +
           std::to_string(PASSWORD_HASH_ITERATIONS) + "$" +
           encodeHex(salt.data(), salt.size()) + "$" +
           encodeHex(derivedKey.data(), derivedKey.size());
#elif defined(__linux__) || defined(__APPLE__)
    std::array<unsigned char, PASSWORD_SALT_BYTES> salt{};
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
        throw std::runtime_error("OpenSSL could not generate a secure password salt.");
    }
    const std::vector<unsigned char> saltVector(salt.begin(), salt.end());
    const std::vector<unsigned char> derivedKey =
        derivePasswordKey(password, saltVector, PASSWORD_HASH_ITERATIONS);
    return std::string(PASSWORD_HASH_PREFIX) +
           std::to_string(PASSWORD_HASH_ITERATIONS) + "$" +
           encodeHex(salt.data(), salt.size()) + "$" +
           encodeHex(derivedKey.data(), derivedKey.size());
#else
    (void)password;
    throw std::runtime_error("Secure passenger password hashing is unavailable on this platform.");
#endif
}

bool verifyPassword(const std::string& storedPassword,
                    const std::string& candidatePassword) {
    if (!hasPasswordHashPrefix(storedPassword)) {
        return constantTimeEqual(storedPassword, candidatePassword);
    }
    const ParsedPasswordHash parsed = parsePasswordHash(storedPassword);
    const std::vector<unsigned char> candidate =
        derivePasswordKey(candidatePassword, parsed.salt, parsed.iterations);
    if (candidate.size() != parsed.hash.size()) return false;
    unsigned char difference = 0;
    for (std::size_t index = 0; index < candidate.size(); ++index) {
        difference |= candidate[index] ^ parsed.hash[index];
    }
    return difference == 0;
}

template <typename Writer>
void writeAtomically(const std::filesystem::path& target,
                     const std::string& errorMessage, Writer writer) {
#ifdef _WIN32
    static std::atomic<unsigned long long> sequence{0};
    std::filesystem::path temporary = target;
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
                 std::to_wstring(sequence.fetch_add(1));
    std::fstream file(temporary, std::ios::out | std::ios::trunc);
    if (!file) throw std::runtime_error(errorMessage);
    try {
        writer(file);
        file.flush();
        if (!file) throw std::runtime_error(errorMessage);
        file.close();
        if (!file) throw std::runtime_error(errorMessage);
        if (!MoveFileExW(temporary.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(), errorMessage);
        }
    } catch (...) {
        if (file.is_open()) file.close();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
#elif defined(__linux__) || defined(__APPLE__)
    static std::atomic<unsigned long long> sequence{0};
    std::filesystem::path temporary = target;
    temporary += ".tmp." + std::to_string(static_cast<unsigned long long>(getpid())) +
                 "." + std::to_string(sequence.fetch_add(1));
    std::fstream file(temporary, std::ios::out | std::ios::trunc);
    if (!file) throw std::runtime_error(errorMessage);
    try {
        writer(file);
        file.flush();
        if (!file) throw std::runtime_error(errorMessage);
        file.close();
        if (!file) throw std::runtime_error(errorMessage);
        std::error_code error;
        std::filesystem::rename(temporary, target, error);
        if (error) throw std::system_error(error, errorMessage);
    } catch (...) {
        if (file.is_open()) file.close();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
#else
    (void)target;
    (void)writer;
    throw std::runtime_error(errorMessage + " Atomic replacement is unavailable.");
#endif
}

std::vector<std::string> loadRecordLines(PostgresStore* store,
                                         const std::string& recordType,
                                         const std::filesystem::path& path) {
    if (store != nullptr) {
        std::vector<std::string> lines = store->loadRecords(recordType);
        for (std::string& line : lines) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
        }
        return lines;
    }
    std::fstream file(path, std::ios::in);
    if (!file && std::filesystem::exists(path)) {
        throw std::runtime_error("Could not open " + path.string());
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    if (file.bad()) throw std::runtime_error("Error reading " + path.string());
    return lines;
}

void saveRecordLines(PostgresStore* store, const std::string& recordType,
                     const std::filesystem::path& path,
                     const std::vector<std::string>& records,
                     const std::string& errorMessage) {
    if (store != nullptr) {
        store->replaceRecords(recordType, records);
        return;
    }
    writeAtomically(path, errorMessage, [&](std::fstream& file) {
        for (const std::string& record : records) file << record << '\n';
    });
}
}  // namespace

User::User(std::string id, std::string name, std::string password)
    : id_(std::move(id)), name_(std::move(name)), password_(std::move(password)) {}

const std::string& User::getId() const { return id_; }
const std::string& User::getName() const { return name_; }
bool User::authenticate(const std::string& password) const {
    return verifyPassword(password_, password);
}

Passenger::Passenger(std::string id, std::string name, std::string contact,
                     std::string password, std::string preferredTravelClass,
                     std::string dietaryPreference, std::string accessibilityPreference)
    : User(std::move(id), std::move(name), std::move(password)),
      contact_(std::move(contact)),
      preferredTravelClass_(std::move(preferredTravelClass)),
      dietaryPreference_(std::move(dietaryPreference)),
      accessibilityPreference_(std::move(accessibilityPreference)) {}

const std::string& Passenger::getContact() const { return contact_; }
const std::string& Passenger::getPreferredTravelClass() const {
    return preferredTravelClass_;
}
const std::string& Passenger::getDietaryPreference() const {
    return dietaryPreference_;
}
const std::string& Passenger::getAccessibilityPreference() const {
    return accessibilityPreference_;
}

Admin::Admin(std::string username, std::string password)
    : User(std::move(username), "Administrator", std::move(password)) {}

std::ostream& operator<<(std::ostream& output, const Flight& flight) {
    output << std::left << std::setw(11) << flight.number
           << std::setw(19) << flight.airline << std::setw(15) << flight.source
           << std::setw(15) << flight.destination << std::setw(13) << flight.date
           << std::setw(9) << flight.departureTime << std::setw(9) << flight.arrivalTime
           << std::right << std::setw(11) << money(flight.baseFare)
           << std::setw(11) << money(flight.baseFare * BUSINESS_MULTIPLIER);
    return output;
}

Ticket::Ticket(std::string pnr, std::string passengerId, std::string passengerName,
               const Flight& flight, double baseFare, int passengerCount)
    : pnr_(std::move(pnr)), passengerId_(std::move(passengerId)),
      passengerName_(std::move(passengerName)), flight_(flight), baseFare_(baseFare),
      passengerCount_(passengerCount) {}

const std::string& Ticket::getPnr() const { return pnr_; }
const std::string& Ticket::getPassengerId() const { return passengerId_; }
const std::string& Ticket::getPassengerName() const { return passengerName_; }
const Flight& Ticket::getFlight() const { return flight_; }
double Ticket::getBaseFare() const { return baseFare_; }
int Ticket::getPassengerCount() const { return passengerCount_; }

double EconomyTicket::calculateFare() const {
    return baseFare_ * static_cast<double>(passengerCount_);
}
std::string EconomyTicket::className() const { return "Economy"; }
double BusinessTicket::calculateFare() const {
    return baseFare_ * BUSINESS_MULTIPLIER * static_cast<double>(passengerCount_);
}
std::string BusinessTicket::className() const { return "Business"; }

std::ostream& operator<<(std::ostream& output, const Ticket& ticket) {
    output << "\n+----------------------------------------------------------------------------+\n"
           << "|                         AIRLINE E-TICKET                                   |\n"
           << "+----------------------------------------------------------------------------+\n"
           << "| PNR: " << std::left << std::setw(27) << ticket.pnr_
           << " Passenger: " << std::setw(32) << ticket.passengerName_ << "|\n"
           << "| Passenger ID: " << std::setw(25) << ticket.passengerId_
           << " Class: " << std::setw(29) << ticket.className() << "|\n"
           << "| Flight: " << std::setw(15) << ticket.flight_.number
           << " Airline: " << std::setw(42) << ticket.flight_.airline << "|\n"
           << "| Route: " << std::setw(24) << ticket.flight_.source + " -> " +
                                     ticket.flight_.destination
           << " Date: " << std::setw(27) << ticket.flight_.date << "|\n"
           << "| Departure: " << std::setw(14) << ticket.flight_.departureTime
           << " Fare: " << std::setw(37)
           << ("INR " + money(ticket.calculateFare())) << "|\n"
           << "+----------------------------------------------------------------------------+\n";
    return output;
}

unsigned long long AirlineReservationSystem::nextPnr_ = 100000;

AirlineReservationSystem::AirlineReservationSystem(
    std::string dataDirectory, std::string adminUsername,
    std::string adminPassword, std::string databaseUrl,
    std::string schemaFile)
    : dataDirectory_(std::move(dataDirectory)),
      admin_(std::move(adminUsername), std::move(adminPassword)) {
    std::filesystem::create_directories(dataDirectory_);
    if (!databaseUrl.empty()) {
        postgresStore_ = std::make_unique<PostgresStore>(databaseUrl);
        postgresStore_->initializeAndImport(dataDirectory_, schemaFile);
    }
    loadData();
    if (passengerDataMigrated_) {
        savePassengers();
        passengerDataMigrated_ = false;
    }
    if (flights_.empty()) {
        seedFlights();
        saveFlights();
    }
    if ((postgresStore_ != nullptr && passengers_.empty()) ||
        (postgresStore_ == nullptr &&
         !std::filesystem::exists(std::filesystem::path(dataDirectory_) / "passengers.txt"))) {
        savePassengers();
    }
    if ((postgresStore_ != nullptr && bookings_.empty()) ||
        (postgresStore_ == nullptr &&
         !std::filesystem::exists(std::filesystem::path(dataDirectory_) / "bookings.txt"))) {
        saveBookings();
    }
    initializePnrCounter();
}

AirlineReservationSystem::~AirlineReservationSystem() = default;

void AirlineReservationSystem::loadData() {
    const std::filesystem::path flightsPath =
        std::filesystem::path(dataDirectory_) / "flights.txt";
    const std::vector<std::string> flightLines =
        loadRecordLines(postgresStore_.get(), "flights", flightsPath);
    for (const std::string& line : flightLines) {
        if (trim(line).empty()) continue;
        try {
            const auto fields = split(line);
            if (fields.size() != 9 && fields.size() != 10) {
                throw std::runtime_error("Expected 9 or 10 flight fields.");
            }
            Flight flight;
            flight.number = fields[0];
            flight.airline = fields[1];
            flight.source = fields[2];
            flight.destination = fields[3];
            flight.date = fields[4];
            flight.departureTime = fields[5];
            if (fields.size() == 10) {
                flight.arrivalTime = fields[6];
                flight.baseFare = parseDouble(fields[7]);
                flight.economySeats = parseInt(fields[8]);
                flight.businessSeats = parseInt(fields[9]);
            } else {
                flight.arrivalTime = estimateArrival(flight.departureTime);
                flight.baseFare = parseDouble(fields[6]);
                flight.economySeats = parseInt(fields[7]);
                flight.businessSeats = parseInt(fields[8]);
            }
            flights_.push_back(flight);
        } catch (const std::exception& error) {
            throw std::runtime_error("Could not read " + flightsPath.string() + ": " +
                                     error.what());
        }
    }

    const std::filesystem::path passengersPath =
        std::filesystem::path(dataDirectory_) / "passengers.txt";
    const std::vector<std::string> passengerLines =
        loadRecordLines(postgresStore_.get(), "passengers", passengersPath);
    for (const std::string& line : passengerLines) {
        if (trim(line).empty()) continue;
        try {
            const auto fields = split(line);
            if (fields.size() != 4 && fields.size() != 7) {
                throw std::runtime_error("Expected 4 or 7 passenger fields.");
            }
            validateText(fields[0], "Passenger ID", 40);
            validateText(fields[1], "Passenger name", 100);
            validateText(fields[2], "Passenger contact", 120);
            if (findPassengerUnlocked(fields[0]) != nullptr) {
                throw std::runtime_error("Duplicate passenger ID in data file.");
            }
            std::string storedPassword = fields[3];
            if (hasPasswordHashPrefix(storedPassword)) {
                (void)parsePasswordHash(storedPassword);
            } else {
                validateText(storedPassword, "Passenger password", 200);
                storedPassword = hashPassword(storedPassword);
                passengerDataMigrated_ = true;
            }
            if (fields.size() == 7) {
                if (!fields[4].empty()) validateText(fields[4], "Travel class", 20);
                if (!fields[5].empty()) validateText(fields[5], "Dietary preference", 120);
                if (!fields[6].empty()) {
                    validateText(fields[6], "Accessibility preference", 120);
                }
            }
            passengers_.emplace_back(
                fields[0], fields[1], fields[2], std::move(storedPassword),
                fields.size() == 7 ? fields[4] : "E",
                fields.size() == 7 ? fields[5] : "",
                fields.size() == 7 ? fields[6] : "");
        } catch (const std::exception& error) {
            throw std::runtime_error("Could not read " + passengersPath.string() + ": " +
                                     error.what());
        }
    }

    const std::filesystem::path bookingsPath =
        std::filesystem::path(dataDirectory_) / "bookings.txt";
    const std::vector<std::string> bookingLines =
        loadRecordLines(postgresStore_.get(), "bookings", bookingsPath);
    for (const std::string& line : bookingLines) {
        if (trim(line).empty()) continue;
        try {
            const auto fields = split(line);
            if (fields.size() != 9 && fields.size() != 17 && fields.size() != 24) {
                throw std::runtime_error("Expected 9, 17 or 24 booking fields.");
            }
            Booking booking;
            booking.pnr = fields[0];
            booking.passengerId = fields[1];
            booking.passengerName = fields[2];
            booking.flight.number = fields[3];
            booking.flight.date = fields[4];
            booking.travelClass = fields[5];
            booking.baseFare = parseDouble(fields[6]);
            booking.farePaid = parseDouble(fields[7]);
            if (fields[8] != "0" && fields[8] != "1") {
                throw std::runtime_error("Invalid booking cancellation flag.");
            }
            booking.cancelled = fields[8] == "1";
            if (fields.size() == 17) {
                booking.passengerCount = parseInt(fields[9]);
                booking.flight.airline = fields[10];
                booking.flight.source = fields[11];
                booking.flight.destination = fields[12];
                booking.flight.departureTime = fields[13];
                booking.flight.arrivalTime = fields[14];
                booking.flight.economySeats = parseInt(fields[15]);
                booking.flight.businessSeats = parseInt(fields[16]);
            } else if (fields.size() == 24) {
                booking.passengerCount = parseInt(fields[9]);
                booking.flight.airline = fields[10];
                booking.flight.source = fields[11];
                booking.flight.destination = fields[12];
                booking.flight.departureTime = fields[13];
                booking.flight.arrivalTime = fields[14];
                booking.flight.economySeats = parseInt(fields[15]);
                booking.flight.businessSeats = parseInt(fields[16]);
                booking.seatNumbers = splitList(fields[17]);
                booking.addOns = splitList(fields[18]);
                booking.promoCode = fields[19];
                booking.seatCharges = parseDouble(fields[20]);
                booking.addOnCharges = parseDouble(fields[21]);
                booking.discountAmount = parseDouble(fields[22]);
                booking.refundAmount = parseDouble(fields[23]);
            } else {
                const Flight* flight = findFlightUnlocked(booking.flight.number);
                if (flight != nullptr) booking.flight = *flight;
                booking.passengerCount = 1;
            }
            if (fields.size() != 24 && booking.cancelled) {
                booking.refundAmount = booking.farePaid;
            }
            if (booking.passengerCount < 1 || booking.passengerCount > MAX_PARTY_SIZE) {
                throw std::runtime_error("Invalid passenger count in booking.");
            }
            bookings_.push_back(booking);
        } catch (const std::exception& error) {
            throw std::runtime_error("Could not read " + bookingsPath.string() + ": " +
                                     error.what());
        }
    }
}

void AirlineReservationSystem::saveFlights() const {
    const std::filesystem::path path =
        std::filesystem::path(dataDirectory_) / "flights.txt";
    std::vector<std::string> records;
    records.reserve(flights_.size());
    for (const Flight& flight : flights_) {
        std::ostringstream line;
        line << std::fixed << std::setprecision(2)
             << flight.number << FIELD_SEPARATOR << flight.airline << FIELD_SEPARATOR
             << flight.source << FIELD_SEPARATOR << flight.destination << FIELD_SEPARATOR
             << flight.date << FIELD_SEPARATOR << flight.departureTime << FIELD_SEPARATOR
             << flight.arrivalTime << FIELD_SEPARATOR << flight.baseFare
             << FIELD_SEPARATOR << flight.economySeats << FIELD_SEPARATOR
             << flight.businessSeats;
        records.push_back(line.str());
    }
    saveRecordLines(postgresStore_.get(), "flights", path, records,
                    "Could not save flights file.");
}

void AirlineReservationSystem::savePassengers() const {
    const std::filesystem::path path =
        std::filesystem::path(dataDirectory_) / "passengers.txt";
    std::vector<std::string> records;
    records.reserve(passengers_.size());
    for (const Passenger& passenger : passengers_) {
        std::ostringstream line;
        line << passenger.getId() << FIELD_SEPARATOR << passenger.getName()
             << FIELD_SEPARATOR << passenger.getContact() << FIELD_SEPARATOR
             << passenger.password_ << FIELD_SEPARATOR
             << passenger.getPreferredTravelClass() << FIELD_SEPARATOR
             << passenger.getDietaryPreference() << FIELD_SEPARATOR
             << passenger.getAccessibilityPreference();
        records.push_back(line.str());
    }
    saveRecordLines(postgresStore_.get(), "passengers", path, records,
                    "Could not save passengers file.");
}

void AirlineReservationSystem::saveBookings() const {
    const std::filesystem::path path =
        std::filesystem::path(dataDirectory_) / "bookings.txt";
    std::vector<std::string> records;
    records.reserve(bookings_.size());
    for (const Booking& booking : bookings_) {
        std::ostringstream line;
        line << std::fixed << std::setprecision(2)
             << booking.pnr << FIELD_SEPARATOR << booking.passengerId << FIELD_SEPARATOR
             << booking.passengerName << FIELD_SEPARATOR << booking.flight.number
             << FIELD_SEPARATOR << booking.flight.date << FIELD_SEPARATOR
             << booking.travelClass << FIELD_SEPARATOR << booking.baseFare
             << FIELD_SEPARATOR << booking.farePaid << FIELD_SEPARATOR
             << (booking.cancelled ? 1 : 0) << FIELD_SEPARATOR << booking.passengerCount
             << FIELD_SEPARATOR << booking.flight.airline << FIELD_SEPARATOR
             << booking.flight.source << FIELD_SEPARATOR << booking.flight.destination
             << FIELD_SEPARATOR << booking.flight.departureTime << FIELD_SEPARATOR
             << booking.flight.arrivalTime << FIELD_SEPARATOR << booking.flight.economySeats
             << FIELD_SEPARATOR << booking.flight.businessSeats << FIELD_SEPARATOR
             << joinList(booking.seatNumbers) << FIELD_SEPARATOR
             << joinList(booking.addOns) << FIELD_SEPARATOR << booking.promoCode
             << FIELD_SEPARATOR << booking.seatCharges << FIELD_SEPARATOR
             << booking.addOnCharges << FIELD_SEPARATOR << booking.discountAmount
             << FIELD_SEPARATOR << booking.refundAmount;
        records.push_back(line.str());
    }
    saveRecordLines(postgresStore_.get(), "bookings", path, records,
                    "Could not save bookings file.");
}

void AirlineReservationSystem::seedFlights() {
    flights_ = {
        {"AI204", "Air India", "Delhi", "Mumbai", "2026-12-15", "08:30", "10:55",
         5500, 40, 8},
        {"6E517", "IndiGo", "Mumbai", "Bengaluru", "2026-12-15", "11:15", "12:55",
         4200, 50, 10},
        {"UK832", "Vistara", "Delhi", "Bengaluru", "2026-12-16", "17:45", "20:30",
         6800, 30, 6},
        {"AI619", "Air India", "Chennai", "Delhi", "2026-12-17", "06:20", "09:10",
         6100, 35, 7}
    };
}

void AirlineReservationSystem::initializePnrCounter() {
    for (const Booking& booking : bookings_) {
        if (booking.pnr.size() <= 3 || booking.pnr.compare(0, 3, "ARS") != 0) continue;
        try {
            const unsigned long long value = std::stoull(booking.pnr.substr(3));
            if (value < std::numeric_limits<unsigned long long>::max()) {
                nextPnr_ = std::max(nextPnr_, value + 1);
            }
        } catch (const std::exception&) {
        }
    }
}

std::string AirlineReservationSystem::generatePnr() {
    for (;;) {
        const std::string pnr = "ARS" + std::to_string(nextPnr_++);
        const auto existing = std::find_if(bookings_.begin(), bookings_.end(),
            [&pnr](const Booking& booking) { return booking.pnr == pnr; });
        if (existing == bookings_.end()) return pnr;
    }
}

const Flight* AirlineReservationSystem::findFlightUnlocked(
    const std::string& flightNumber) const {
    const auto flight = std::find_if(flights_.begin(), flights_.end(),
        [&flightNumber](const Flight& item) { return item.number == flightNumber; });
    return flight == flights_.end() ? nullptr : &*flight;
}

Passenger* AirlineReservationSystem::findPassengerUnlocked(const std::string& passengerId) {
    const auto passenger = std::find_if(passengers_.begin(), passengers_.end(),
        [&passengerId](const Passenger& item) { return item.getId() == passengerId; });
    return passenger == passengers_.end() ? nullptr : &*passenger;
}

const Passenger* AirlineReservationSystem::findPassengerUnlocked(
    const std::string& passengerId) const {
    const auto passenger = std::find_if(passengers_.begin(), passengers_.end(),
        [&passengerId](const Passenger& item) { return item.getId() == passengerId; });
    return passenger == passengers_.end() ? nullptr : &*passenger;
}

int AirlineReservationSystem::activeSeatsUnlocked(const std::string& flightNumber,
                                                  const std::string& travelClass) const {
    int total = 0;
    for (const Booking& booking : bookings_) {
        if (!booking.cancelled && booking.flight.number == flightNumber &&
            booking.travelClass == travelClass) {
            total += booking.passengerCount;
        }
    }
    return total;
}

std::vector<SeatInfo> AirlineReservationSystem::getSeatMapUnlocked(
    const Flight& flight, const std::string& travelClass) const {
    const int capacity = travelClass == "E" ? flight.economySeats : flight.businessSeats;
    const std::string letters = travelClass == "E" ? "ABCDEF" : "ABCD";
    const int seatsPerRow = static_cast<int>(letters.size());
    std::vector<SeatInfo> seats;
    seats.reserve(static_cast<std::size_t>(capacity));
    for (int index = 0; index < capacity; ++index) {
        const int row = index / seatsPerRow + 1;
        const char letter = letters[static_cast<std::size_t>(index % seatsPerRow)];
        SeatInfo seat;
        seat.number = std::to_string(row) + letter;
        if (travelClass == "E" && row == 1) {
            seat.type = "Front row";
            seat.surcharge = 500;
        } else if (travelClass == "E" && row == 5) {
            seat.type = "Exit row";
            seat.surcharge = 900;
        } else if (letter == 'A' || letter == letters.back()) {
            seat.type = "Window";
        } else if (travelClass == "E" ? (letter == 'C' || letter == 'D')
                                      : (letter == 'B' || letter == 'C')) {
            seat.type = "Aisle";
        } else {
            seat.type = "Middle";
        }
        seats.push_back(std::move(seat));
    }

    for (const Booking& booking : bookings_) {
        if (booking.cancelled || booking.flight.number != flight.number ||
            booking.travelClass != travelClass) {
            continue;
        }
        int legacySeatsToAssign = booking.passengerCount;
        for (const std::string& number : booking.seatNumbers) {
            const auto seat = std::find_if(seats.begin(), seats.end(),
                [&number](const SeatInfo& item) { return item.number == number; });
            if (seat != seats.end() && !seat->occupied) {
                seat->occupied = true;
                --legacySeatsToAssign;
            }
        }
        for (SeatInfo& seat : seats) {
            if (legacySeatsToAssign <= 0) break;
            if (!seat.occupied) {
                seat.occupied = true;
                --legacySeatsToAssign;
            }
        }
    }
    return seats;
}

PassengerProfile AirlineReservationSystem::makePassengerProfileUnlocked(
    const Passenger& passenger) const {
    int miles = 0;
    for (const Booking& booking : bookings_) {
        if (booking.passengerId == passenger.getId() && !booking.cancelled) {
            miles += static_cast<int>(std::floor(booking.farePaid / 100.0));
        }
    }
    const std::string tier = miles >= 10000 ? "Platinum"
        : miles >= 2500 ? "Gold" : "Silver";
    return PassengerProfile{passenger.getId(), passenger.getName(), passenger.getContact(),
                            passenger.getPreferredTravelClass(),
                            passenger.getDietaryPreference(),
                            passenger.getAccessibilityPreference(), miles, tier};
}

FlightSearchResult AirlineReservationSystem::makeSearchResult(
    const Flight& flight, int availableEconomy, int availableBusiness) {
    return FlightSearchResult{flight, availableEconomy, availableBusiness};
}

PassengerProfile AirlineReservationSystem::registerPassenger(
    const std::string& id, const std::string& name, const std::string& contact,
    const std::string& password) {
    validateText(id, "Passenger ID", 40);
    validateText(name, "Passenger name", 100);
    validateText(contact, "Contact", 120);
    validateText(password, "Password", 200);
    if (password.size() < 12) {
        throw std::invalid_argument("Password must contain at least 12 characters.");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (findPassengerUnlocked(id) != nullptr) {
        throw std::logic_error("A passenger with that ID is already registered.");
    }
    passengers_.emplace_back(id, name, contact, hashPassword(password));
    try {
        savePassengers();
    } catch (...) {
        passengers_.pop_back();
        throw;
    }
    return makePassengerProfileUnlocked(passengers_.back());
}

PassengerProfile AirlineReservationSystem::authenticatePassenger(
    const std::string& id, const std::string& password) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Passenger* passenger = findPassengerUnlocked(id);
    if (passenger == nullptr || !passenger->authenticate(password)) {
        throw std::invalid_argument("Invalid passenger ID or password.");
    }
    return makePassengerProfileUnlocked(*passenger);
}

bool AirlineReservationSystem::authenticateAdmin(const std::string& username,
                                                 const std::string& password) const {
    return admin_.getId() == username && admin_.authenticate(password);
}

PassengerProfile AirlineReservationSystem::getPassengerProfile(
    const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Passenger* passenger = findPassengerUnlocked(id);
    if (passenger == nullptr) throw std::out_of_range("Passenger not found.");
    return makePassengerProfileUnlocked(*passenger);
}

PassengerProfile AirlineReservationSystem::updatePassengerProfile(
    const std::string& id, const std::string& name, const std::string& contact,
    const std::string& preferredTravelClass, const std::string& dietaryPreference,
    const std::string& accessibilityPreference) {
    validateText(name, "Passenger name", 100);
    validateText(contact, "Contact", 120);
    if (!dietaryPreference.empty()) validateText(dietaryPreference, "Dietary preference", 120);
    if (!accessibilityPreference.empty()) {
        validateText(accessibilityPreference, "Accessibility preference", 120);
    }
    std::string preferredClass = lower(trim(preferredTravelClass));
    if (preferredClass == "economy" || preferredClass == "e") preferredClass = "E";
    else if (preferredClass == "business" || preferredClass == "b") preferredClass = "B";
    else throw std::invalid_argument("Preferred class must be Economy or Business.");

    std::lock_guard<std::mutex> lock(mutex_);
    Passenger* passenger = findPassengerUnlocked(id);
    if (passenger == nullptr) throw std::out_of_range("Passenger not found.");
    const std::string oldName = passenger->name_;
    const std::string oldContact = passenger->contact_;
    const std::string oldClass = passenger->preferredTravelClass_;
    const std::string oldDietary = passenger->dietaryPreference_;
    const std::string oldAccessibility = passenger->accessibilityPreference_;
    passenger->name_ = name;
    passenger->contact_ = contact;
    passenger->preferredTravelClass_ = preferredClass;
    passenger->dietaryPreference_ = dietaryPreference;
    passenger->accessibilityPreference_ = accessibilityPreference;
    try {
        savePassengers();
    } catch (...) {
        passenger->name_ = oldName;
        passenger->contact_ = oldContact;
        passenger->preferredTravelClass_ = oldClass;
        passenger->dietaryPreference_ = oldDietary;
        passenger->accessibilityPreference_ = oldAccessibility;
        throw;
    }
    return makePassengerProfileUnlocked(*passenger);
}

std::vector<FlightSearchResult> AirlineReservationSystem::searchFlights(
    const std::string& source, const std::string& destination) const {
    return searchFlights(source, destination, "", 1);
}

std::vector<FlightSearchResult> AirlineReservationSystem::searchFlights(
    const std::string& source, const std::string& destination,
    const std::string& date, int passengerCount) const {
    if (passengerCount < 1 || passengerCount > MAX_PARTY_SIZE) {
        throw std::invalid_argument("Passenger count must be between 1 and 9.");
    }
    if (!date.empty() && !validDate(date)) {
        throw std::invalid_argument("Travel date must be a real date in YYYY-MM-DD format.");
    }

    const std::string sourceKey = lower(trim(source));
    const std::string destinationKey = lower(trim(destination));
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<FlightSearchResult> results;
    for (const Flight& flight : flights_) {
        if ((!sourceKey.empty() && lower(flight.source) != sourceKey) ||
            (!destinationKey.empty() && lower(flight.destination) != destinationKey) ||
            (!date.empty() && flight.date != date)) {
            continue;
        }
        const int economyAvailable = std::max(
            0, flight.economySeats - activeSeatsUnlocked(flight.number, "E"));
        const int businessAvailable = std::max(
            0, flight.businessSeats - activeSeatsUnlocked(flight.number, "B"));
        results.push_back(makeSearchResult(flight, economyAvailable, businessAvailable));
    }
    return results;
}

std::vector<Flight> AirlineReservationSystem::getAllFlights() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return flights_;
}

Flight AirlineReservationSystem::getFlight(const std::string& flightNumber) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Flight* flight = findFlightUnlocked(flightNumber);
    if (flight == nullptr) throw std::out_of_range("Flight not found.");
    return *flight;
}

std::vector<SeatInfo> AirlineReservationSystem::getSeatMap(
    const std::string& flightNumber, const std::string& travelClass) const {
    std::string cabin = lower(trim(travelClass));
    if (cabin == "economy" || cabin == "e") cabin = "E";
    else if (cabin == "business" || cabin == "b") cabin = "B";
    else throw std::invalid_argument("Travel class must be Economy or Business.");
    std::lock_guard<std::mutex> lock(mutex_);
    const Flight* flight = findFlightUnlocked(flightNumber);
    if (flight == nullptr) throw std::out_of_range("Flight not found.");
    return getSeatMapUnlocked(*flight, cabin);
}

Booking AirlineReservationSystem::createBooking(const std::string& passengerId,
                                                const std::string& flightNumber,
                                                const std::string& travelClass,
                                                int passengerCount,
                                                const std::vector<std::string>& seatNumbers,
                                                const std::vector<std::string>& addOns,
                                                const std::string& promoCode) {
    if (passengerCount < 1 || passengerCount > MAX_PARTY_SIZE) {
        throw std::invalid_argument("Passenger count must be between 1 and 9.");
    }
    std::string cabin = lower(trim(travelClass));
    if (cabin == "economy" || cabin == "e") cabin = "E";
    else if (cabin == "business" || cabin == "b") cabin = "B";
    else throw std::invalid_argument("Travel class must be Economy or Business.");

    std::lock_guard<std::mutex> lock(mutex_);
    const Passenger* passenger = findPassengerUnlocked(passengerId);
    if (passenger == nullptr) throw std::out_of_range("Passenger not found.");
    const Flight* flight = findFlightUnlocked(flightNumber);
    if (flight == nullptr) throw std::out_of_range("Flight not found.");

    const int available = cabin == "E"
        ? flight->economySeats - activeSeatsUnlocked(flightNumber, "E")
        : flight->businessSeats - activeSeatsUnlocked(flightNumber, "B");
    if (available < passengerCount) {
        throw std::logic_error("Not enough seats are available in that class.");
    }

    std::vector<SeatInfo> seatMap = getSeatMapUnlocked(*flight, cabin);
    std::vector<std::string> assignedSeats = seatNumbers;
    if (assignedSeats.empty()) {
        for (const SeatInfo& seat : seatMap) {
            if (!seat.occupied) assignedSeats.push_back(seat.number);
            if (static_cast<int>(assignedSeats.size()) == passengerCount) break;
        }
    }
    if (static_cast<int>(assignedSeats.size()) != passengerCount) {
        throw std::invalid_argument("Select exactly one seat for each passenger.");
    }
    std::set<std::string> uniqueSeats;
    double seatCharges = 0.0;
    for (const std::string& number : assignedSeats) {
        if (!uniqueSeats.insert(number).second) {
            throw std::invalid_argument("A seat cannot be selected more than once.");
        }
        const auto seat = std::find_if(seatMap.begin(), seatMap.end(),
            [&number](const SeatInfo& item) { return item.number == number; });
        if (seat == seatMap.end()) throw std::invalid_argument("Selected seat is not valid.");
        if (seat->occupied) throw std::logic_error("A selected seat is no longer available.");
        seatCharges += seat->surcharge;
    }

    const std::map<std::string, double> addOnPrices = {
        {"extra-bag", 1500.0}, {"priority", 750.0}, {"lounge", 2500.0},
        {"meal", 550.0}, {"insurance", 900.0}
    };
    std::set<std::string> uniqueAddOns;
    double addOnCharges = 0.0;
    for (const std::string& addOn : addOns) {
        if (!uniqueAddOns.insert(addOn).second) {
            throw std::invalid_argument("An add-on cannot be selected more than once.");
        }
        const auto price = addOnPrices.find(addOn);
        if (price == addOnPrices.end()) {
            throw std::invalid_argument("One or more selected add-ons are not available.");
        }
        addOnCharges += price->second * static_cast<double>(passengerCount);
    }

    const std::string normalizedPromo = lower(trim(promoCode)) == "skyward10"
        ? "SKYWARD10" : trim(promoCode);
    if (!normalizedPromo.empty() && normalizedPromo != "SKYWARD10") {
        throw std::invalid_argument("That promotional code is not valid.");
    }

    const std::string pnr = generatePnr();
    std::unique_ptr<Ticket> ticket;
    if (cabin == "E") {
        ticket = std::make_unique<EconomyTicket>(
            pnr, passenger->getId(), passenger->getName(), *flight,
            flight->baseFare, passengerCount);
    } else {
        ticket = std::make_unique<BusinessTicket>(
            pnr, passenger->getId(), passenger->getName(), *flight,
            flight->baseFare, passengerCount);
    }
    const double baseCharge = ticket->calculateFare();
    const double discountAmount = normalizedPromo.empty() ? 0.0 : baseCharge * 0.10;
    Booking booking;
    booking.pnr = pnr;
    booking.passengerId = passenger->getId();
    booking.passengerName = passenger->getName();
    booking.flight = *flight;
    booking.travelClass = cabin;
    booking.baseFare = ticket->getBaseFare();
    booking.farePaid = baseCharge + seatCharges + addOnCharges - discountAmount;
    booking.passengerCount = passengerCount;
    booking.seatNumbers = std::move(assignedSeats);
    booking.addOns = addOns;
    booking.promoCode = normalizedPromo;
    booking.seatCharges = seatCharges;
    booking.addOnCharges = addOnCharges;
    booking.discountAmount = discountAmount;
    bookings_.push_back(booking);
    try {
        saveBookings();
    } catch (...) {
        bookings_.pop_back();
        throw;
    }
    return booking;
}

Booking AirlineReservationSystem::cancelBooking(const std::string& pnr,
                                                const std::string& passengerId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto booking = std::find_if(bookings_.begin(), bookings_.end(),
        [&pnr](const Booking& item) { return item.pnr == pnr; });
    if (booking == bookings_.end()) throw std::out_of_range("Booking not found.");
    if (booking->passengerId != passengerId) {
        throw std::invalid_argument("This booking does not belong to this passenger.");
    }
    if (booking->cancelled) throw std::logic_error("This booking is already cancelled.");
    booking->cancelled = true;
    booking->refundAmount = booking->farePaid;
    try {
        saveBookings();
    } catch (...) {
        booking->cancelled = false;
        booking->refundAmount = 0.0;
        throw;
    }
    return *booking;
}

Booking AirlineReservationSystem::getBooking(const std::string& pnr) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto booking = std::find_if(bookings_.begin(), bookings_.end(),
        [&pnr](const Booking& item) { return item.pnr == pnr; });
    if (booking == bookings_.end()) throw std::out_of_range("Booking not found.");
    return *booking;
}

std::vector<Booking> AirlineReservationSystem::getBookingsForPassenger(
    const std::string& passengerId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Booking> matches;
    for (const Booking& booking : bookings_) {
        if (booking.passengerId == passengerId) matches.push_back(booking);
    }
    return matches;
}

std::vector<Booking> AirlineReservationSystem::getAllBookings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bookings_;
}

std::vector<PassengerProfile> AirlineReservationSystem::getAllPassengerProfiles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PassengerProfile> profiles;
    profiles.reserve(passengers_.size());
    for (const Passenger& passenger : passengers_) {
        profiles.push_back(makePassengerProfileUnlocked(passenger));
    }
    return profiles;
}

void AirlineReservationSystem::addFlight(Flight flight) {
    validateFlight(flight);
    std::lock_guard<std::mutex> lock(mutex_);
    if (findFlightUnlocked(flight.number) != nullptr) {
        throw std::logic_error("A flight with that number already exists.");
    }
    flights_.push_back(std::move(flight));
    try {
        saveFlights();
    } catch (...) {
        flights_.pop_back();
        throw;
    }
}

Flight AirlineReservationSystem::updateFlight(const std::string& flightNumber,
                                              Flight updatedFlight) {
    updatedFlight.number = flightNumber;
    validateFlight(updatedFlight);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto flight = std::find_if(flights_.begin(), flights_.end(),
        [&flightNumber](const Flight& item) { return item.number == flightNumber; });
    if (flight == flights_.end()) throw std::out_of_range("Flight not found.");
    if (updatedFlight.economySeats < activeSeatsUnlocked(flightNumber, "E") ||
        updatedFlight.businessSeats < activeSeatsUnlocked(flightNumber, "B")) {
        throw std::logic_error("Seat capacity cannot be less than active bookings.");
    }
    const Flight original = *flight;
    *flight = std::move(updatedFlight);
    try {
        saveFlights();
    } catch (...) {
        *flight = original;
        throw;
    }
    return *flight;
}

void AirlineReservationSystem::deleteFlight(const std::string& flightNumber) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto flight = std::find_if(flights_.begin(), flights_.end(),
        [&flightNumber](const Flight& item) { return item.number == flightNumber; });
    if (flight == flights_.end()) throw std::out_of_range("Flight not found.");
    if (activeSeatsUnlocked(flightNumber, "E") + activeSeatsUnlocked(flightNumber, "B") > 0) {
        throw std::logic_error("A flight with active bookings cannot be deleted.");
    }
    const std::size_t index = static_cast<std::size_t>(flight - flights_.begin());
    const Flight removed = *flight;
    flights_.erase(flight);
    try {
        saveFlights();
    } catch (...) {
        flights_.insert(flights_.begin() + static_cast<std::ptrdiff_t>(index), removed);
        throw;
    }
}

AdminStatistics AirlineReservationSystem::getAdminStatistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    AdminStatistics stats;
    stats.totalFlights = flights_.size();
    stats.registeredPassengers = passengers_.size();
    stats.totalBookings = bookings_.size();
    for (const Flight& flight : flights_) {
        stats.totalSeats += flight.economySeats + flight.businessSeats;
        stats.reservedSeats += activeSeatsUnlocked(flight.number, "E") +
                               activeSeatsUnlocked(flight.number, "B");
    }
    for (const Booking& booking : bookings_) {
        if (booking.cancelled) {
            ++stats.cancelledBookings;
        } else {
            ++stats.activeBookings;
            stats.activeRevenue += booking.farePaid;
        }
    }
    if (stats.totalSeats > 0) {
        stats.occupancyPercent =
            100.0 * static_cast<double>(stats.reservedSeats) /
            static_cast<double>(stats.totalSeats);
    }
    return stats;
}
