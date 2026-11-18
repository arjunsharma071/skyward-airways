#ifndef SKYWARD_SESSION_REGISTRY_HPP
#define SKYWARD_SESSION_REGISTRY_HPP

#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

struct Session {
    std::string id;
    std::string role;
    std::string name;
};

class SessionRegistry {
public:
    using Clock = std::chrono::steady_clock;

    explicit SessionRegistry(std::chrono::seconds idleTimeout =
                                 std::chrono::minutes(30),
                             std::size_t maximumSessions = 10000)
        : idleTimeout_(idleTimeout), maximumSessions_(maximumSessions) {
        if (idleTimeout_ <= std::chrono::seconds::zero() || maximumSessions_ == 0) {
            throw std::invalid_argument("Session registry limits must be positive.");
        }
    }

    void store(const std::string& token, Session session,
               Clock::time_point now = Clock::now()) {
        if (token.empty()) throw std::invalid_argument("Session token cannot be empty.");
        std::lock_guard<std::mutex> lock(mutex_);
        removeExpired(now);
        const auto existing = sessions_.find(token);
        if (existing == sessions_.end() && sessions_.size() >= maximumSessions_) {
            throw std::runtime_error("The maximum number of active sessions was reached.");
        }
        sessions_[token] = Entry{std::move(session), now + idleTimeout_};
    }

    std::optional<Session> authenticate(const std::string& token,
                                        Clock::time_point now = Clock::now()) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = sessions_.find(token);
        if (found == sessions_.end()) return std::nullopt;
        if (found->second.expiresAt <= now) {
            sessions_.erase(found);
            return std::nullopt;
        }
        found->second.expiresAt = now + idleTimeout_;
        return found->second.session;
    }

    void erase(const std::string& token) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(token);
    }

private:
    struct Entry {
        Session session;
        Clock::time_point expiresAt;
    };

    void removeExpired(Clock::time_point now) {
        for (auto item = sessions_.begin(); item != sessions_.end();) {
            if (item->second.expiresAt <= now) item = sessions_.erase(item);
            else ++item;
        }
    }

    std::chrono::seconds idleTimeout_;
    std::size_t maximumSessions_;
    std::map<std::string, Entry> sessions_;
    std::mutex mutex_;
};

#endif
