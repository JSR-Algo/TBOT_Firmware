#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Application-owned: serialize Start, Cancel and Poll on the application task.
// Only the last token is retained. The owner must reject older identities using
// current connection/generation/reset epochs before Start; IDs must be unique.
class ConversationPlayoutDrain {
public:
    static constexpr size_t kMaxDrainIdBytes = 128;
    static constexpr uint64_t kTimeoutUs = 10'000'000;

    enum class State { Idle, Pending, Complete, Cancelled, TimedOut };
    enum class Action { None, Complete, Cancelled, TimedOut };

    struct Token {
        uint64_t connection_epoch = 0;
        uint32_t response_generation = 0;
        uint64_t reset_epoch = 0;
        std::string_view drain_id;
    };

    struct Snapshot {
        uint64_t connection_epoch = 0;
        uint32_t response_generation = 0;
        uint64_t reset_epoch = 0;
        // Capture a stable token ID with the snapshot; this is correlation, not
        // device completion proof. Epochs and hardware_drained provide that fence.
        std::string_view drain_id;
        bool stopped = false;
        size_t decode_queue_count = 0;
        size_t playback_queue_count = 0;
        bool decode_in_flight = false;
        bool output_in_flight = false;
        bool hardware_drained = false;
    };

    bool Start(uint64_t now_us, const Token& token) {
        if (token.drain_id.size() <= 5 || token.drain_id.size() > kMaxDrainIdBytes ||
            token.drain_id.substr(0, 5) != "chat:" ||
            token.drain_id.find('\0') != std::string_view::npos) {
            return false;
        }
        // Retries of the last token are accepted without restarting its lifecycle.
        if (state_ != State::Idle && Matches(token.connection_epoch,
                token.response_generation, token.reset_epoch, token.drain_id)) {
            return true;
        }
        // Copy before replacing storage: callers may pass a view of DrainId().
        std::array<char, kMaxDrainIdBytes + 1> id{};
        token.drain_id.copy(id.data(), token.drain_id.size());
        drain_id_ = id;
        drain_id_size_ = token.drain_id.size();
        connection_epoch_ = token.connection_epoch;
        response_generation_ = token.response_generation;
        reset_epoch_ = token.reset_epoch;
        started_us_ = now_us;
        cancel_requested_ = false;
        state_ = State::Pending;
        return true;
    }

    void Cancel() {
        if (state_ == State::Pending) cancel_requested_ = true;
    }

    Action Poll(uint64_t now_us, const Snapshot& snapshot) {
        if (state_ != State::Pending) return Action::None;
        if (cancel_requested_ || snapshot.stopped ||
            !Matches(snapshot.connection_epoch, snapshot.response_generation,
                     snapshot.reset_epoch, snapshot.drain_id)) {
            state_ = State::Cancelled;
            return Action::Cancelled;
        }
        // Elapsed arithmetic avoids deadline overflow; callers use a monotonic clock.
        if (now_us - started_us_ >= kTimeoutUs) {
            state_ = State::TimedOut;
            return Action::TimedOut;
        }
        if (snapshot.decode_queue_count == 0 && snapshot.playback_queue_count == 0 &&
            !snapshot.decode_in_flight && !snapshot.output_in_flight &&
            snapshot.hardware_drained) {
            state_ = State::Complete;
            return Action::Complete;
        }
        return Action::None;
    }

    State GetState() const { return state_; }
    std::string_view DrainId() const { return {drain_id_.data(), drain_id_size_}; }

private:
    bool Matches(uint64_t connection_epoch, uint32_t response_generation,
                 uint64_t reset_epoch, std::string_view drain_id) const {
        return connection_epoch == connection_epoch_ &&
            response_generation == response_generation_ && reset_epoch == reset_epoch_ &&
            drain_id == DrainId();
    }

    State state_ = State::Idle;
    std::array<char, kMaxDrainIdBytes + 1> drain_id_{};
    size_t drain_id_size_ = 0;
    uint64_t connection_epoch_ = 0;
    uint32_t response_generation_ = 0;
    uint64_t reset_epoch_ = 0;
    uint64_t started_us_ = 0;
    bool cancel_requested_ = false;
};
