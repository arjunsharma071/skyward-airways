#include "AirlineReservationSystem.hpp"
#include "SessionRegistry.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Exception, typename Operation>
void expectThrows(Operation operation, const std::string& message) {
    try {
        operation();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("skyward-tests-" + std::to_string(ticks));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

Flight testFlight() {
    return {"TST100", "Test Airways", "Test Origin", "Test Destination",
            "2031-02-03", "09:00", "11:00", 1200.0, 1, 1};
}

void testFilePersistenceAndBookingFlow() {
    TemporaryDirectory temporary;
    const std::string passengerPassword = "Test-Password-2031";
    std::string firstPnr;
    std::string secondPnr;

    {
        AirlineReservationSystem system(temporary.path().string(), "test-admin",
                                        "SyntheticAdminPassword");
        check(system.authenticateAdmin("test-admin", "SyntheticAdminPassword"),
              "Configured administrator credentials should authenticate.");
        system.addFlight(testFlight());
        system.registerPassenger("TEST-P1", "Synthetic Passenger One",
                                 "one@example.invalid", passengerPassword);
        system.registerPassenger("TEST-P2", "Synthetic Passenger Two",
                                 "two@example.invalid", passengerPassword);
        expectThrows<std::logic_error>(
            [&] {
                system.registerPassenger("TEST-P1", "Duplicate Passenger",
                                         "duplicate@example.invalid",
                                         "Another-Password-2031");
            },
            "Duplicate passenger IDs must be rejected.");
        check(system.authenticatePassenger("TEST-P1", passengerPassword).id == "TEST-P1",
              "A registered passenger should be able to sign in.");
        expectThrows<std::invalid_argument>(
            [&] { system.authenticatePassenger("TEST-P1", "Incorrect-Password"); },
            "Incorrect passenger passwords must be rejected.");
        check(system.searchFlights("Test Origin", "Test Destination",
                                   "2031-02-03", 1).size() == 1,
              "Flight search should return the matching schedule.");

        const Booking first = system.createBooking("TEST-P1", "TST100", "E", 1);
        firstPnr = first.pnr;
        check(!firstPnr.empty(), "Booking must receive a PNR.");
        check(system.getSeatMap("TST100", "E").front().occupied,
              "A booked seat should be shown as occupied.");
        expectThrows<std::logic_error>(
            [&] { system.createBooking("TEST-P2", "TST100", "E", 1); },
            "The backend must reject an overbooking attempt.");

        const Booking cancelled = system.cancelBooking(firstPnr, "TEST-P1");
        check(cancelled.cancelled && cancelled.refundAmount == cancelled.farePaid,
              "Cancellation should record the full demonstration refund.");
        check(!system.getSeatMap("TST100", "E").front().occupied,
              "Cancellation should release the seat.");
        expectThrows<std::logic_error>(
            [&] { system.cancelBooking(firstPnr, "TEST-P1"); },
            "Repeated cancellation must be rejected.");

        const Booking second = system.createBooking("TEST-P2", "TST100", "E", 1);
        secondPnr = second.pnr;
        check(firstPnr != secondPnr, "Bookings must receive distinct PNRs.");
        check(system.getBooking(secondPnr).passengerId == "TEST-P2",
              "Ticket retrieval should return its passenger and booking.");
    }

    {
        AirlineReservationSystem restarted(temporary.path().string(), "test-admin",
                                           "SyntheticAdminPassword");
        check(restarted.authenticatePassenger("TEST-P1", passengerPassword).id == "TEST-P1",
              "Passenger records must persist after a restart.");
        check(restarted.getBooking(firstPnr).cancelled,
              "Cancellation state must persist after a restart.");
        check(!restarted.getBooking(secondPnr).cancelled,
              "Active bookings must persist after a restart.");
        check(restarted.getSeatMap("TST100", "E").front().occupied,
              "The remaining active booking should keep its seat reserved.");
    }
}

void writeFlightData(const std::filesystem::path& directory,
                     const std::string& flightRecord,
                     const std::string& lineEnding) {
    std::filesystem::create_directories(directory);
    {
        std::ofstream flights(directory / "flights.txt", std::ios::binary);
        flights << flightRecord << lineEnding;
        if (!flights) throw std::runtime_error("Could not create synthetic flight fixture.");
    }
    for (const char* fileName : {"passengers.txt", "bookings.txt"}) {
        std::ofstream file(directory / fileName, std::ios::binary);
        if (!file) throw std::runtime_error("Could not create empty synthetic data file.");
    }
}

void testCrLfFlightRecords() {
    const std::string validFlight =
        "CRLF100|CRLF Airways|Origin|Destination|2031-02-03|09:00|11:00|"
        "1200.00|2|1";

    {
        TemporaryDirectory temporary;
        writeFlightData(temporary.path(), validFlight, "\r\n");
        AirlineReservationSystem system(temporary.path().string(), "test-admin",
                                        "SyntheticAdminPassword");
        const auto flights = system.searchFlights("Origin", "Destination", "2031-02-03");
        check(flights.size() == 1 && flights.front().flight.number == "CRLF100",
              "A valid CRLF-terminated flight record should load and remain searchable.");
        check(flights.front().availableEconomy == 2 &&
                  flights.front().availableBusiness == 1,
              "CRLF flight seat counts should parse without changing their values.");
    }

    {
        TemporaryDirectory temporary;
        writeFlightData(
            temporary.path(),
            "BADINT|Strict Airways|Origin|Destination|2031-02-03|09:00|11:00|"
            "1200.00|2|1x",
            "\r\n");
        expectThrows<std::runtime_error>(
            [&] {
                AirlineReservationSystem system(temporary.path().string(), "test-admin",
                                                "SyntheticAdminPassword");
            },
            "A CRLF flight record with a malformed integer must still be rejected.");
    }
}

void testSessionRegistry() {
    using Clock = SessionRegistry::Clock;
    const Clock::time_point start{};
    SessionRegistry sessions(std::chrono::seconds(5), 2);
    sessions.store("synthetic-token", Session{"TEST-P1", "passenger", "Synthetic"},
                   start);

    const auto active = sessions.authenticate("synthetic-token", start +
                                               std::chrono::seconds(4));
    check(active && active->id == "TEST-P1",
          "A session should authenticate before its idle timeout.");
    check(sessions.authenticate("synthetic-token", start +
                                std::chrono::seconds(8)).has_value(),
          "Session activity should refresh the idle timeout.");
    check(!sessions.authenticate("synthetic-token", start +
                                 std::chrono::seconds(13)).has_value(),
          "An idle session should expire and be rejected.");

    sessions.store("logout-token", Session{"TEST-P2", "passenger", "Synthetic"},
                   start);
    sessions.erase("logout-token");
    check(!sessions.authenticate("logout-token", start).has_value(),
          "Logging out should immediately invalidate the session.");

    SessionRegistry limited(std::chrono::seconds(5), 1);
    limited.store("first-token", Session{"TEST-P1", "passenger", "Synthetic"}, start);
    expectThrows<std::runtime_error>(
        [&] {
            limited.store("second-token",
                          Session{"TEST-P2", "passenger", "Synthetic"}, start);
        },
        "The session registry must enforce its configured capacity.");
}

void writeLegacyFixture(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    {
        std::ofstream flights(directory / "flights.txt", std::ios::binary);
        flights << "DBT100|Database Test Airways|Legacy Origin|Legacy Destination|"
                   "2031-02-03|09:00|11:00|1200.00|1|1\n";
        if (!flights) throw std::runtime_error("Could not create synthetic flight fixture.");
    }
    {
        std::ofstream passengers(directory / "passengers.txt", std::ios::binary);
        passengers << "LEGACY-P|Legacy Synthetic Passenger|legacy@example.invalid|"
                      "Legacy-Password-2031\n";
        if (!passengers) throw std::runtime_error("Could not create synthetic passenger fixture.");
    }
    {
        std::ofstream bookings(directory / "bookings.txt", std::ios::binary);
        if (!bookings) throw std::runtime_error("Could not create synthetic booking fixture.");
    }
}

void testPostgresImportAndRestart(const std::string& databaseUrl) {
    TemporaryDirectory temporary;
    const std::filesystem::path dataDirectory = temporary.path() / "data";
    writeLegacyFixture(dataDirectory);
    const std::filesystem::path schema =
        std::filesystem::current_path() / "database" / "schema.sql";

    {
        AirlineReservationSystem system(dataDirectory.string(), "test-admin",
                                        "SyntheticAdminPassword", databaseUrl, schema.string());
        check(system.authenticatePassenger("LEGACY-P", "Legacy-Password-2031").id == "LEGACY-P",
              "PostgreSQL import must retain access to a legacy passenger.");
        system.createBooking("LEGACY-P", "DBT100", "E", 1);
    }

    std::ifstream originalPassengerFile(dataDirectory / "passengers.txt",
                                        std::ios::binary);
    std::string originalPassengerLine;
    std::getline(originalPassengerFile, originalPassengerLine);
    check(originalPassengerLine.find("Legacy-Password-2031") != std::string::npos,
          "Database migration must not modify the source legacy file.");

    {
        AirlineReservationSystem restarted(dataDirectory.string(), "test-admin",
                                           "SyntheticAdminPassword", databaseUrl, schema.string());
        check(restarted.authenticatePassenger("LEGACY-P", "Legacy-Password-2031").id ==
                  "LEGACY-P",
              "PostgreSQL passenger data must persist after restart.");
        const auto bookings = restarted.getBookingsForPassenger("LEGACY-P");
        check(bookings.size() == 1 && !bookings.front().cancelled,
              "PostgreSQL booking data must persist after restart.");
        const Booking cancelled =
            restarted.cancelBooking(bookings.front().pnr, "LEGACY-P");
        check(cancelled.cancelled,
              "PostgreSQL cancellation should persist through the database.");
    }
}

}  // namespace

int main() {
    try {
        testFilePersistenceAndBookingFlow();
        std::cout << "[pass] file persistence, authentication, booking, overbooking, "
                     "cancellation, and seat release\n";
        testCrLfFlightRecords();
        std::cout << "[pass] CRLF flight parsing and strict integer validation\n";
        testSessionRegistry();
        std::cout << "[pass] sliding session expiration, logout invalidation, and capacity\n";

        const char* databaseUrl = std::getenv("TEST_DATABASE_URL");
        if (databaseUrl != nullptr && *databaseUrl != '\0') {
            testPostgresImportAndRestart(databaseUrl);
            std::cout << "[pass] PostgreSQL legacy import, source preservation, "
                         "booking, and restart persistence\n";
        } else {
            std::cout << "[skip] PostgreSQL integration (TEST_DATABASE_URL not set)\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
