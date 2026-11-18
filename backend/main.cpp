#define CPPHTTPLIB_THREAD_POOL_COUNT 8
#include "AirlineReservationSystem.hpp"
#include "SessionRegistry.hpp"
#include "httplib.h"
#include "json.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <openssl/rand.h>
#endif

namespace {
using Json = nlohmann::json;

struct ApiError : std::runtime_error {
    ApiError(int statusCode, const std::string& message)
        : std::runtime_error(message), status(statusCode) {}
    int status;
};

SessionRegistry sessions;
volatile std::sig_atomic_t shutdownSignal = 0;

void handleShutdownSignal(int signalNumber) {
    shutdownSignal = signalNumber;
}

std::string environmentValue(const char* name, const std::string& fallback = "") {
    const char* value = std::getenv(name);
    return value == nullptr ? fallback : std::string(value);
}

int configuredPort(const std::string& value) {
    std::size_t consumed = 0;
    int port = 0;
    try {
        port = std::stoi(value, &consumed);
    } catch (const std::exception&) {
        throw std::invalid_argument("SERVER_PORT must be an integer from 1 to 65535.");
    }
    if (consumed != value.size() || port < 1 || port > 65535) {
        throw std::invalid_argument("SERVER_PORT must be an integer from 1 to 65535.");
    }
    return port;
}

std::string createToken() {
#ifdef _WIN32
    std::array<unsigned char, 32> bytes{};
    const NTSTATUS status = BCryptGenRandom(
        nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) throw std::runtime_error("Could not create a secure session token.");
    static const char digits[] = "0123456789abcdef";
    std::string token;
    token.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        token.push_back(digits[byte >> 4]);
        token.push_back(digits[byte & 0x0F]);
    }
    return token;
#elif defined(__linux__) || defined(__APPLE__)
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("Could not create a secure session token.");
    }
    static const char digits[] = "0123456789abcdef";
    std::string token;
    token.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        token.push_back(digits[byte >> 4]);
        token.push_back(digits[byte & 0x0F]);
    }
    return token;
#else
    throw std::runtime_error("Secure session tokens are unavailable on this platform.");
#endif
}

void saveSession(const std::string& token, const Session& session) {
    sessions.store(token, session);
}

void removeSession(const std::string& token) {
    sessions.erase(token);
}

Session requireSession(const httplib::Request& request, const std::string& requiredRole = "") {
    const std::string header = request.get_header_value("Authorization");
    const std::string prefix = "Bearer ";
    if (header.compare(0, prefix.size(), prefix) != 0) {
        throw ApiError(401, "Please sign in to continue.");
    }
    const std::string token = header.substr(prefix.size());
    const std::optional<Session> session = sessions.authenticate(token);
    if (!session) throw ApiError(401, "Your session has expired. Sign in again.");
    if (!requiredRole.empty() && session->role != requiredRole) {
        throw ApiError(403, "You do not have permission to perform this action.");
    }
    return *session;
}

Json parseBody(const httplib::Request& request) {
    try {
        Json body = Json::parse(request.body);
        if (!body.is_object()) throw ApiError(400, "The request body must be a JSON object.");
        return body;
    } catch (const nlohmann::json::parse_error&) {
        throw ApiError(400, "The request body must contain valid JSON.");
    }
}

std::string requiredString(const Json& body, const char* key) {
    const auto field = body.find(key);
    if (field == body.end() || !field->is_string()) {
        throw ApiError(400, std::string("The '") + key + "' field is required.");
    }
    return field->get<std::string>();
}

int optionalInt(const Json& body, const char* key, int fallback) {
    const auto field = body.find(key);
    if (field == body.end()) return fallback;
    if (field->is_number_integer()) return field->get<int>();
    throw ApiError(400, std::string("The '") + key + "' field must be an integer.");
}

std::string optionalString(const Json& body, const char* key,
                           const std::string& fallback = "") {
    const auto field = body.find(key);
    if (field == body.end()) return fallback;
    if (field->is_string()) return field->get<std::string>();
    throw ApiError(400, std::string("The '") + key + "' field must be a string.");
}

std::vector<std::string> optionalStringArray(const Json& body, const char* key) {
    const auto field = body.find(key);
    if (field == body.end()) return {};
    if (!field->is_array()) {
        throw ApiError(400, std::string("The '") + key + "' field must be an array.");
    }
    std::vector<std::string> values;
    values.reserve(field->size());
    for (const Json& value : *field) {
        if (!value.is_string()) {
            throw ApiError(400, std::string("Every item in '") + key + "' must be a string.");
        }
        values.push_back(value.get<std::string>());
    }
    return values;
}

double requiredNumber(const Json& body, const char* key) {
    const auto field = body.find(key);
    if (field == body.end() || !field->is_number()) {
        throw ApiError(400, std::string("The '") + key + "' field must be a number.");
    }
    return field->get<double>();
}

Json toJson(const PassengerProfile& passenger) {
    return Json{{"id", passenger.id},
                {"name", passenger.name},
                {"contact", passenger.contact},
                {"preferredTravelClass", passenger.preferredTravelClass == "B"
                    ? "Business" : "Economy"},
                {"dietaryPreference", passenger.dietaryPreference},
                {"accessibilityPreference", passenger.accessibilityPreference},
                {"loyaltyMiles", passenger.loyaltyMiles},
                {"eliteTier", passenger.eliteTier}};
}

Json toJson(const Flight& flight, int availableEconomy, int availableBusiness) {
    return Json{{"number", flight.number},
                {"airline", flight.airline},
                {"source", flight.source},
                {"destination", flight.destination},
                {"date", flight.date},
                {"departureTime", flight.departureTime},
                {"arrivalTime", flight.arrivalTime},
                {"baseFare", flight.baseFare},
                {"economyFare", flight.baseFare},
                {"businessFare", flight.baseFare * 1.8},
                {"economySeats", flight.economySeats},
                {"businessSeats", flight.businessSeats},
                {"availableEconomy", availableEconomy},
                {"availableBusiness", availableBusiness}};
}

Json toJson(const Flight& flight) {
    return toJson(flight, flight.economySeats, flight.businessSeats);
}

Json toJson(const Booking& booking) {
    return Json{{"pnr", booking.pnr},
                {"passengerId", booking.passengerId},
                {"passengerName", booking.passengerName},
                {"flight", toJson(booking.flight)},
                {"travelClass", booking.travelClass == "B" ? "Business" : "Economy"},
                {"baseFare", booking.baseFare},
                {"farePaid", booking.farePaid},
                {"passengerCount", booking.passengerCount},
                {"seatNumbers", booking.seatNumbers},
                {"addOns", booking.addOns},
                {"promoCode", booking.promoCode},
                {"seatCharges", booking.seatCharges},
                {"addOnCharges", booking.addOnCharges},
                {"discountAmount", booking.discountAmount},
                {"refundAmount", booking.refundAmount},
                {"status", booking.cancelled ? "Cancelled" : "Confirmed"},
                {"cancelled", booking.cancelled}};
}

Json toJson(const SeatInfo& seat) {
    return Json{{"number", seat.number},
                {"type", seat.type},
                {"surcharge", seat.surcharge},
                {"occupied", seat.occupied}};
}

Json toJson(const AdminStatistics& stats) {
    return Json{{"totalFlights", stats.totalFlights},
                {"registeredPassengers", stats.registeredPassengers},
                {"totalBookings", stats.totalBookings},
                {"activeBookings", stats.activeBookings},
                {"cancelledBookings", stats.cancelledBookings},
                {"totalSeats", stats.totalSeats},
                {"reservedSeats", stats.reservedSeats},
                {"occupancyPercent", stats.occupancyPercent},
                {"activeRevenue", stats.activeRevenue}};
}

void sendJson(httplib::Response& response, int status, const Json& value) {
    response.status = status;
    response.set_content(value.dump(), "application/json; charset=utf-8");
}

template <typename Handler>
auto apiRoute(Handler handler) {
    return [handler = std::move(handler)](const httplib::Request& request,
                                          httplib::Response& response) {
        try {
            handler(request, response);
        } catch (const ApiError& error) {
            sendJson(response, error.status, Json{{"error", error.what()}});
        } catch (const std::invalid_argument& error) {
            sendJson(response, 400, Json{{"error", error.what()}});
        } catch (const std::out_of_range& error) {
            sendJson(response, 404, Json{{"error", error.what()}});
        } catch (const std::logic_error& error) {
            sendJson(response, 409, Json{{"error", error.what()}});
        } catch (const std::exception& error) {
            std::cerr << "[error] " << error.what() << '\n';
            sendJson(response, 500, Json{{"error", "An unexpected server error occurred."}});
        }
    };
}

Flight flightFromJson(const Json& body) {
    Flight flight;
    flight.number = requiredString(body, "number");
    flight.airline = requiredString(body, "airline");
    flight.source = requiredString(body, "source");
    flight.destination = requiredString(body, "destination");
    flight.date = requiredString(body, "date");
    flight.departureTime = requiredString(body, "departureTime");
    flight.arrivalTime = requiredString(body, "arrivalTime");
    flight.baseFare = requiredNumber(body, "baseFare");
    flight.economySeats = optionalInt(body, "economySeats", -1);
    flight.businessSeats = optionalInt(body, "businessSeats", -1);
    return flight;
}

void registerRoutes(httplib::Server& server, AirlineReservationSystem& system,
                    const std::filesystem::path& frontendPath) {
    server.Get("/api/health", apiRoute([](const auto&, auto& response) {
        sendJson(response, 200, Json{{"status", "ok"},
                                     {"service", "Airline Reservation System"}});
    }));

    server.Post("/api/auth/register", apiRoute([&system](const auto& request, auto& response) {
        const Json body = parseBody(request);
        const std::string token = createToken();
        const PassengerProfile passenger = system.registerPassenger(
            requiredString(body, "id"), requiredString(body, "name"),
            requiredString(body, "contact"), requiredString(body, "password"));
        saveSession(token, Session{passenger.id, "passenger", passenger.name});
        sendJson(response, 201, Json{{"token", token},
                                     {"user", toJson(passenger)},
                                     {"role", "passenger"}});
    }));

    server.Post("/api/auth/passenger/login",
        apiRoute([&system](const auto& request, auto& response) {
            const Json body = parseBody(request);
            PassengerProfile passenger;
            try {
                passenger = system.authenticatePassenger(requiredString(body, "id"),
                                                         requiredString(body, "password"));
            } catch (const std::invalid_argument&) {
                throw ApiError(401, "Invalid passenger ID or password.");
            }
            const std::string token = createToken();
            saveSession(token, Session{passenger.id, "passenger", passenger.name});
            sendJson(response, 200, Json{{"token", token},
                                         {"user", toJson(passenger)},
                                         {"role", "passenger"}});
        }));

    server.Post("/api/auth/admin/login",
        apiRoute([&system](const auto& request, auto& response) {
            const Json body = parseBody(request);
            const std::string username = requiredString(body, "username");
            const std::string password = requiredString(body, "password");
            if (!system.authenticateAdmin(username, password)) {
                throw ApiError(401, "Invalid administrator username or password.");
            }
            const std::string token = createToken();
            saveSession(token, Session{"admin", "admin", "Administrator"});
            sendJson(response, 200, Json{{"token", token},
                                         {"user", {{"id", "admin"},
                                                   {"name", "Administrator"},
                                                   {"contact", ""}}},
                                         {"role", "admin"}});
        }));

    server.Get("/api/auth/me", apiRoute([&system](const auto& request, auto& response) {
        const Session session = requireSession(request);
        if (session.role == "admin") {
            sendJson(response, 200, Json{{"user", {{"id", "admin"},
                                                   {"name", "Administrator"},
                                                   {"contact", ""}}},
                                         {"role", "admin"}});
        } else {
            sendJson(response, 200, Json{{"user", toJson(system.getPassengerProfile(session.id))},
                                         {"role", "passenger"}});
        }
    }));

    server.Post("/api/auth/logout", apiRoute([](const auto& request, auto& response) {
        const Session session = requireSession(request);
        (void)session;
        const std::string header = request.get_header_value("Authorization");
        removeSession(header.substr(std::string("Bearer ").size()));
        sendJson(response, 200, Json{{"message", "Signed out successfully."}});
    }));

    server.Get("/api/flights", apiRoute([&system](const auto& request, auto& response) {
        const std::string source = request.has_param("source")
            ? request.get_param_value("source") : "";
        const std::string destination = request.has_param("destination")
            ? request.get_param_value("destination") : "";
        const std::string date = request.has_param("date")
            ? request.get_param_value("date") : "";
        int passengerCount = 1;
        if (request.has_param("passengers")) {
            try {
                passengerCount = std::stoi(request.get_param_value("passengers"));
            } catch (const std::exception&) {
                throw ApiError(400, "Passenger count must be a whole number.");
            }
        }
        const auto flights = system.searchFlights(source, destination, date, passengerCount);
        Json results = Json::array();
        for (const FlightSearchResult& result : flights) {
            results.push_back(toJson(result.flight, result.availableEconomy,
                                     result.availableBusiness));
        }
        sendJson(response, 200, Json{{"flights", std::move(results)}});
    }));

    server.Get("/api/fares/calendar",
        apiRoute([&system](const auto& request, auto& response) {
            const std::string source = request.has_param("source")
                ? request.get_param_value("source") : "";
            const std::string destination = request.has_param("destination")
                ? request.get_param_value("destination") : "";
            const auto flights = system.searchFlights(source, destination);
            std::map<std::string, double> lowestFareByDate;
            for (const FlightSearchResult& result : flights) {
                const double fare = result.flight.baseFare;
                auto date = lowestFareByDate.find(result.flight.date);
                if (date == lowestFareByDate.end() || fare < date->second) {
                    lowestFareByDate[result.flight.date] = fare;
                }
            }
            Json fares = Json::array();
            for (const auto& entry : lowestFareByDate) {
                fares.push_back(Json{{"date", entry.first}, {"economyFare", entry.second}});
            }
            sendJson(response, 200, Json{{"fares", std::move(fares)}});
        }));

    server.Get(R"(/api/flights/([A-Za-z0-9_-]+)/seats)",
        apiRoute([&system](const auto& request, auto& response) {
            const std::string cabin = request.has_param("travelClass")
                ? request.get_param_value("travelClass") : "E";
            Json seats = Json::array();
            for (const SeatInfo& seat : system.getSeatMap(request.matches[1], cabin)) {
                seats.push_back(toJson(seat));
            }
            sendJson(response, 200, Json{{"seats", std::move(seats)}});
        }));

    server.Get(R"(/api/flights/([A-Za-z0-9_-]+)/status)",
        apiRoute([&system](const auto& request, auto& response) {
            const Flight flight = system.getFlight(request.matches[1]);
            const auto now = std::chrono::system_clock::now();
            const std::time_t currentTime = std::chrono::system_clock::to_time_t(now);
            std::tm localTime{};
#ifdef _WIN32
            localtime_s(&localTime, &currentTime);
#else
            localtime_r(&currentTime, &localTime);
#endif
            char currentDate[11]{};
            std::strftime(currentDate, sizeof(currentDate), "%Y-%m-%d", &localTime);
            const std::string status = flight.date < currentDate ? "Completed" : "Scheduled";
            sendJson(response, 200, Json{{"flightNumber", flight.number},
                                         {"date", flight.date},
                                         {"status", status},
                                         {"isDemoProjection", true},
                                         {"notice", "Schedule-based demo status; not a live airline feed."}});
        }));

    server.Post("/api/bookings", apiRoute([&system](const auto& request, auto& response) {
        const Session session = requireSession(request, "passenger");
        const Json body = parseBody(request);
        const Booking booking = system.createBooking(
            session.id, requiredString(body, "flightNumber"),
            requiredString(body, "travelClass"), optionalInt(body, "passengerCount", 1),
            optionalStringArray(body, "seatNumbers"),
            optionalStringArray(body, "addOns"), optionalString(body, "promoCode"));
        sendJson(response, 201, Json{{"booking", toJson(booking)}});
    }));

    server.Put("/api/passengers/me",
        apiRoute([&system](const auto& request, auto& response) {
            const Session session = requireSession(request, "passenger");
            const Json body = parseBody(request);
            const PassengerProfile profile = system.updatePassengerProfile(
                session.id, requiredString(body, "name"), requiredString(body, "contact"),
                requiredString(body, "preferredTravelClass"),
                optionalString(body, "dietaryPreference"),
                optionalString(body, "accessibilityPreference"));
            sendJson(response, 200, Json{{"user", toJson(profile)}});
        }));

    server.Get("/api/bookings/mine",
        apiRoute([&system](const auto& request, auto& response) {
            const Session session = requireSession(request, "passenger");
            const auto bookings = system.getBookingsForPassenger(session.id);
            Json records = Json::array();
            for (const Booking& booking : bookings) records.push_back(toJson(booking));
            sendJson(response, 200, Json{{"bookings", std::move(records)}});
        }));

    const auto bookingHandler = [&system](const auto& request, auto& response) {
        const Session session = requireSession(request);
        const Booking booking = system.getBooking(request.matches[1]);
        if (session.role != "admin" && session.id != booking.passengerId) {
            throw ApiError(403, "This ticket belongs to another passenger.");
        }
        sendJson(response, 200, Json{{"booking", toJson(booking)}});
    };
    server.Get(R"(/api/bookings/([A-Za-z0-9_-]+))", apiRoute(bookingHandler));

    server.Delete(R"(/api/bookings/([A-Za-z0-9_-]+))",
        apiRoute([&system](const auto& request, auto& response) {
            const Session session = requireSession(request, "passenger");
            const Booking booking = system.cancelBooking(request.matches[1], session.id);
            sendJson(response, 200, Json{{"booking", toJson(booking)}});
        }));

    server.Get("/api/admin/flights", apiRoute([&system](const auto& request, auto& response) {
        (void)requireSession(request, "admin");
        Json flights = Json::array();
        for (const Flight& flight : system.getAllFlights()) flights.push_back(toJson(flight));
        sendJson(response, 200, Json{{"flights", std::move(flights)}});
    }));

    server.Post("/api/admin/flights",
        apiRoute([&system](const auto& request, auto& response) {
            (void)requireSession(request, "admin");
            const Flight flight = flightFromJson(parseBody(request));
            system.addFlight(flight);
            sendJson(response, 201, Json{{"flight", toJson(flight)},
                                         {"message", "Flight added successfully."}});
        }));

    server.Put(R"(/api/admin/flights/([A-Za-z0-9_-]+))",
        apiRoute([&system](const auto& request, auto& response) {
            (void)requireSession(request, "admin");
            const Flight updated = system.updateFlight(request.matches[1],
                                                       flightFromJson(parseBody(request)));
            sendJson(response, 200, Json{{"flight", toJson(updated)},
                                         {"message", "Flight updated successfully."}});
        }));

    server.Delete(R"(/api/admin/flights/([A-Za-z0-9_-]+))",
        apiRoute([&system](const auto& request, auto& response) {
            (void)requireSession(request, "admin");
            system.deleteFlight(request.matches[1]);
            sendJson(response, 200, Json{{"message", "Flight deleted successfully."}});
        }));

    server.Get("/api/admin/passengers",
        apiRoute([&system](const auto& request, auto& response) {
            (void)requireSession(request, "admin");
            Json passengers = Json::array();
            for (const PassengerProfile& passenger : system.getAllPassengerProfiles()) {
                passengers.push_back(toJson(passenger));
            }
            sendJson(response, 200, Json{{"passengers", std::move(passengers)}});
        }));

    server.Get("/api/admin/bookings",
        apiRoute([&system](const auto& request, auto& response) {
            (void)requireSession(request, "admin");
            Json bookings = Json::array();
            for (const Booking& booking : system.getAllBookings()) {
                bookings.push_back(toJson(booking));
            }
            sendJson(response, 200, Json{{"bookings", std::move(bookings)}});
        }));

    server.Get("/api/admin/stats",
        apiRoute([&system](const auto& request, auto& response) {
            (void)requireSession(request, "admin");
            sendJson(response, 200, toJson(system.getAdminStatistics()));
        }));

    const std::string indexPath = (frontendPath / "index.html").string();
    server.Get("/", [&indexPath](const auto&, auto& response) {
        std::ifstream index(indexPath, std::ios::binary);
        if (!index) {
            response.status = 500;
            response.set_content("Frontend index.html could not be opened.", "text/plain");
            return;
        }
        std::ostringstream contents;
        contents << index.rdbuf();
        response.set_content(contents.str(), "text/html; charset=utf-8");
    });

    server.set_error_handler([](const auto& request, auto& response) {
        if (request.path.compare(0, 4, "/api") == 0) {
            sendJson(response, response.status,
                     Json{{"error", response.status == 404
                         ? "API endpoint not found." : "Request could not be completed."}});
        }
    });
    server.set_logger([](const auto& request, const auto& response) {
        std::cout << "[http] " << request.method << ' ' << request.path
                  << " -> " << response.status << '\n';
    });
}

void openBrowser(const std::string& url) {
#ifdef _WIN32
    HINSTANCE chrome = ShellExecuteA(nullptr, "open", "chrome.exe",
                                     url.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<std::intptr_t>(chrome) <= 32) {
        ShellExecuteA(nullptr, "open", url.c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    }
#else
    (void)url;
#endif
}
}  // namespace

int main() {
    try {
        const std::filesystem::path projectRoot = std::filesystem::current_path();
        const std::filesystem::path frontendPath = projectRoot / "frontend";
        const std::filesystem::path schemaPath = projectRoot / "database" / "schema.sql";
        if (!std::filesystem::exists(frontendPath / "index.html")) {
            throw std::runtime_error(
                "Run this server from the project root; frontend/index.html was not found.");
        }

        const std::string appEnvironment = environmentValue("APP_ENV", "development");
        const bool production = appEnvironment == "production";
        if (appEnvironment != "development" && !production) {
            throw std::invalid_argument("APP_ENV must be development or production.");
        }
        const std::string databaseUrl = environmentValue("DATABASE_URL");
        if (production && databaseUrl.empty()) {
            throw std::invalid_argument("DATABASE_URL is required when APP_ENV=production.");
        }
        const std::string adminUsername =
            environmentValue("ADMIN_USERNAME", production ? "" : "admin");
        const std::string adminPassword =
            environmentValue("ADMIN_PASSWORD", production ? "" : "admin123");
        if (adminUsername.empty() || adminPassword.empty()) {
            throw std::invalid_argument(
                "ADMIN_USERNAME and ADMIN_PASSWORD must be configured.");
        }
        if (production && adminPassword.size() < 16) {
            throw std::invalid_argument(
                "ADMIN_PASSWORD must contain at least 16 characters in production.");
        }
        const std::string dataDirectory =
            environmentValue("DATA_DIRECTORY", (projectRoot / "data").string());
        AirlineReservationSystem system(dataDirectory, adminUsername, adminPassword,
                                        databaseUrl, schemaPath.string());
        httplib::Server server;
        server.set_payload_max_length(1024 * 1024);
        registerRoutes(server, system, frontendPath);
        if (!server.set_mount_point("/", frontendPath.string())) {
            throw std::runtime_error("Could not mount the frontend directory.");
        }

        const std::string host = environmentValue("SERVER_HOST", "127.0.0.1");
        const int port = configuredPort(environmentValue("SERVER_PORT", "8080"));
        const std::string browserUrl = "http://localhost:" + std::to_string(port) + "/";
        std::atomic<bool> listenReturned(false);
        std::thread browserThread([&server, &listenReturned, &browserUrl]() {
            for (int attempt = 0; attempt < 100; ++attempt) {
                if (server.is_running()) {
                    openBrowser(browserUrl);
                    return;
                }
                if (listenReturned.load()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });

        shutdownSignal = 0;
        std::signal(SIGINT, handleShutdownSignal);
#ifdef SIGTERM
        std::signal(SIGTERM, handleShutdownSignal);
#endif
        std::thread shutdownThread([&server]() {
            while (shutdownSignal == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            server.stop();
        });

        std::cout << "[server] Airline Reservation System starting...\n";
        std::cout << "[server] Data directory: " << dataDirectory << '\n';
        std::cout << "[server] Listening on " << host << ':' << port << '\n';
        const bool listened = server.listen(host, port);
        const bool shuttingDown = shutdownSignal != 0;
        listenReturned.store(true);
        if (browserThread.joinable()) browserThread.join();
        if (!shuttingDown) {
            shutdownSignal = 1;
        }
        if (shutdownThread.joinable()) shutdownThread.join();
        if (!listened && !shuttingDown) {
            throw std::runtime_error("Could not bind the configured HTTP server address.");
        }
        std::cout << "[server] Shutdown complete.\n";
    } catch (const std::exception& error) {
        std::cerr << "[server:error] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
