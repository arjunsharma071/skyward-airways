#ifndef AIRLINE_RESERVATION_SYSTEM_HPP
#define AIRLINE_RESERVATION_SYSTEM_HPP

#include <cstddef>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class PostgresStore;

class User {
public:
    User(std::string id, std::string name, std::string password);
    virtual ~User() = default;

    const std::string& getId() const;
    const std::string& getName() const;
    bool authenticate(const std::string& password) const;

protected:
    std::string id_;
    std::string name_;
    std::string password_;

    friend class AirlineReservationSystem;
};

class Passenger : public User {
public:
    Passenger(std::string id, std::string name, std::string contact, std::string password,
              std::string preferredTravelClass = "E",
              std::string dietaryPreference = "",
              std::string accessibilityPreference = "");
    const std::string& getContact() const;
    const std::string& getPreferredTravelClass() const;
    const std::string& getDietaryPreference() const;
    const std::string& getAccessibilityPreference() const;

private:
    std::string contact_;
    std::string preferredTravelClass_;
    std::string dietaryPreference_;
    std::string accessibilityPreference_;

    friend class AirlineReservationSystem;
};

class Admin : public User {
public:
    Admin(std::string username, std::string password);
};

struct PassengerProfile {
    std::string id;
    std::string name;
    std::string contact;
    std::string preferredTravelClass = "E";
    std::string dietaryPreference;
    std::string accessibilityPreference;
    int loyaltyMiles = 0;
    std::string eliteTier = "Silver";
};

struct Flight {
    std::string number;
    std::string airline;
    std::string source;
    std::string destination;
    std::string date;
    std::string departureTime;
    std::string arrivalTime;
    double baseFare = 0.0;
    int economySeats = 0;
    int businessSeats = 0;
};

std::ostream& operator<<(std::ostream& output, const Flight& flight);

class Ticket {
public:
    Ticket(std::string pnr, std::string passengerId, std::string passengerName,
           const Flight& flight, double baseFare, int passengerCount);
    virtual ~Ticket() = default;

    virtual double calculateFare() const = 0;
    virtual std::string className() const = 0;

    const std::string& getPnr() const;
    const std::string& getPassengerId() const;
    const std::string& getPassengerName() const;
    const Flight& getFlight() const;
    double getBaseFare() const;
    int getPassengerCount() const;

    friend std::ostream& operator<<(std::ostream& output, const Ticket& ticket);

protected:
    std::string pnr_;
    std::string passengerId_;
    std::string passengerName_;
    Flight flight_;
    double baseFare_;
    int passengerCount_;
};

class EconomyTicket : public Ticket {
public:
    using Ticket::Ticket;
    double calculateFare() const override;
    std::string className() const override;
};

class BusinessTicket : public Ticket {
public:
    using Ticket::Ticket;
    double calculateFare() const override;
    std::string className() const override;
};

struct FlightSearchResult {
    Flight flight;
    int availableEconomy = 0;
    int availableBusiness = 0;
};

struct SeatInfo {
    std::string number;
    std::string type;
    int surcharge = 0;
    bool occupied = false;
};

struct Booking {
    std::string pnr;
    std::string passengerId;
    std::string passengerName;
    Flight flight;
    std::string travelClass;
    double baseFare = 0.0;
    double farePaid = 0.0;
    int passengerCount = 1;
    bool cancelled = false;
    std::vector<std::string> seatNumbers;
    std::vector<std::string> addOns;
    std::string promoCode;
    double seatCharges = 0.0;
    double addOnCharges = 0.0;
    double discountAmount = 0.0;
    double refundAmount = 0.0;
};

struct AdminStatistics {
    std::size_t totalFlights = 0;
    std::size_t registeredPassengers = 0;
    std::size_t totalBookings = 0;
    std::size_t activeBookings = 0;
    std::size_t cancelledBookings = 0;
    int totalSeats = 0;
    int reservedSeats = 0;
    double occupancyPercent = 0.0;
    double activeRevenue = 0.0;
};

class AirlineReservationSystem {
public:
    AirlineReservationSystem(std::string dataDirectory,
                             std::string adminUsername,
                             std::string adminPassword,
                             std::string databaseUrl = "",
                             std::string schemaFile = "database/schema.sql");
    ~AirlineReservationSystem();

    PassengerProfile registerPassenger(const std::string& id, const std::string& name,
                                       const std::string& contact,
                                       const std::string& password);
    PassengerProfile authenticatePassenger(const std::string& id,
                                           const std::string& password) const;
    bool authenticateAdmin(const std::string& username,
                           const std::string& password) const;
    PassengerProfile getPassengerProfile(const std::string& id) const;
    PassengerProfile updatePassengerProfile(const std::string& id,
                                            const std::string& name,
                                            const std::string& contact,
                                            const std::string& preferredTravelClass,
                                            const std::string& dietaryPreference,
                                            const std::string& accessibilityPreference);

    std::vector<FlightSearchResult> searchFlights(const std::string& source,
                                                  const std::string& destination) const;
    std::vector<FlightSearchResult> searchFlights(const std::string& source,
                                                  const std::string& destination,
                                                  const std::string& date,
                                                  int passengerCount = 1) const;
    std::vector<Flight> getAllFlights() const;
    Flight getFlight(const std::string& flightNumber) const;
    std::vector<SeatInfo> getSeatMap(const std::string& flightNumber,
                                     const std::string& travelClass) const;

    Booking createBooking(const std::string& passengerId,
                          const std::string& flightNumber,
                          const std::string& travelClass,
                          int passengerCount,
                          const std::vector<std::string>& seatNumbers = {},
                          const std::vector<std::string>& addOns = {},
                          const std::string& promoCode = "");
    Booking cancelBooking(const std::string& pnr, const std::string& passengerId);
    Booking getBooking(const std::string& pnr) const;
    std::vector<Booking> getBookingsForPassenger(const std::string& passengerId) const;
    std::vector<Booking> getAllBookings() const;

    std::vector<PassengerProfile> getAllPassengerProfiles() const;
    void addFlight(Flight flight);
    Flight updateFlight(const std::string& flightNumber, Flight updatedFlight);
    void deleteFlight(const std::string& flightNumber);
    AdminStatistics getAdminStatistics() const;

private:
    std::string dataDirectory_;
    std::vector<Flight> flights_;
    std::vector<Passenger> passengers_;
    std::vector<Booking> bookings_;
    Admin admin_;
    std::unique_ptr<PostgresStore> postgresStore_;
    mutable std::mutex mutex_;
    bool passengerDataMigrated_ = false;

    static unsigned long long nextPnr_;

    void loadData();
    void saveFlights() const;
    void savePassengers() const;
    void saveBookings() const;
    void seedFlights();
    void initializePnrCounter();
    std::string generatePnr();

    const Flight* findFlightUnlocked(const std::string& flightNumber) const;
    Passenger* findPassengerUnlocked(const std::string& passengerId);
    const Passenger* findPassengerUnlocked(const std::string& passengerId) const;
    int activeSeatsUnlocked(const std::string& flightNumber,
                            const std::string& travelClass) const;
    std::vector<SeatInfo> getSeatMapUnlocked(const Flight& flight,
                                             const std::string& travelClass) const;
    PassengerProfile makePassengerProfileUnlocked(const Passenger& passenger) const;
    static FlightSearchResult makeSearchResult(const Flight& flight,
                                               int availableEconomy,
                                               int availableBusiness);
};

#endif
