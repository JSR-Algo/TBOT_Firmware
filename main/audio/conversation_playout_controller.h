#pragma once

#include "audio_playback_drain_snapshot.h"
#include "conversation_playout_drain.h"

// Serialize all methods on the application task. Effects are owned values for
// deferred workers; this controller performs no I/O, reset, or mic transition.
class ConversationPlayoutController {
public:
    static constexpr uint64_t kTimeoutUs = ConversationPlayoutDrain::kTimeoutUs;
    using Token = ConversationPlayoutDrain::Token;

    struct Ownership {
        uint64_t connection_epoch = 0;
        uint32_t response_generation = 0;
        uint64_t reset_epoch = 0;
        bool stopped = false;
    };

    enum class EffectKind { None, SubmitAck, Complete, Cancel, Recover };
    enum class Delivery { Busy, Stale, Sent, Failed };

    struct Effect {
        EffectKind kind = EffectKind::None;
        uint64_t request_id = 0;
        uint64_t connection_epoch = 0;
        uint32_t response_generation = 0;
        uint64_t reset_epoch = 0;
        std::array<char, ConversationPlayoutDrain::kMaxDrainIdBytes + 1> drain_id{};
        size_t drain_id_size = 0;

        Token token() const {
            return {connection_epoch, response_generation, reset_epoch,
                    {drain_id.data(), drain_id_size}};
        }
    };

    struct BeginResult {
        bool accepted = false;
        Effect effect;
    };

    // stop_us is the original stop reception time, never the retry/poll time.
    // A replacement returns the old cancellation effect before owning the new
    // token. Deferred consumers must fence effects against current ownership.
    BeginResult Begin(uint64_t stop_us, const Token& token, const Ownership& current) {
        if (!Matches(token, current)) return {};
        if (phase_ != Phase::Idle && SameToken(token)) return {true, {}};
        ConversationPlayoutDrain next;
        if (!next.Start(stop_us, token)) return {};
        Effect cancelled = Active() ? MakeEffect(EffectKind::Cancel) : Effect{};
        drain_ = next;
        owner_ = current;
        started_us_ = stop_us;
        phase_ = Phase::Draining;
        codec_latched_ = false;
        cancel_requested_ = false;
        request_pending_ = false;
        delivery_ready_ = false;
        return {true, cancelled};
    }

    void Cancel() {
        if (Active()) cancel_requested_ = true;
    }

    // Post worker results back to the application task. The request ID fences
    // retries and replacements; the first result for one submission wins.
    void Deliver(uint64_t request_id, Delivery result) {
        if (phase_ != Phase::Ack || !request_pending_ ||
            request_id != request_id_ || delivery_ready_) return;
        delivery_ = result;
        delivery_ready_ = true;
    }

    // A null snapshot means the audio observation try-lock was busy. Even then
    // ownership, cancellation and the original overall deadline are checked.
    Effect Poll(uint64_t now_us, const Ownership& current,
                const PlaybackDrainSnapshot* audio) {
        if (!Active()) return {};
        if (cancel_requested_ || !Matches(CurrentToken(), current)) {
            return Finish(EffectKind::Cancel);
        }
        if (now_us - started_us_ >= kTimeoutUs) return Finish(EffectKind::Recover);
        if (delivery_ready_) {
            if (delivery_ == Delivery::Stale) return Finish(EffectKind::Cancel);
            if (delivery_ == Delivery::Failed) return Finish(EffectKind::Recover);
            if (delivery_ == Delivery::Busy) {
                delivery_ready_ = false;
                request_pending_ = false;
            }
        }
        if (!audio) return {};
        if (audio->stopped || audio->reset_epoch != owner_.reset_epoch ||
            audio->playback_generation != owner_.response_generation) {
            return Finish(EffectKind::Cancel);
        }
        const auto codec_state = audio->codec.state;
        if (codec_state == AudioOutputDrainState::Failed ||
            codec_state == AudioOutputDrainState::Unsupported) {
            return Finish(EffectKind::Recover);
        }
        const bool stable = codec_state != AudioOutputDrainState::Busy;
        const bool quiescent = audio->decode_queue_size == 0 &&
            audio->playback_queue_size == 0 && !audio->decode_in_flight &&
            !audio->output_in_flight;
        if (stable && codec_latched_ && audio->codec.epoch != codec_epoch_) {
            return Finish(EffectKind::Cancel);
        }
        // EnableOutput may advance the codec epoch while queued tail audio is
        // still pending. Latch only the first stable observation after quiescence.
        if (stable && quiescent && !codec_latched_) {
            codec_epoch_ = audio->codec.epoch;
            codec_latched_ = true;
        }
        const bool drained = codec_latched_ && stable &&
            codec_state == AudioOutputDrainState::Drained;
        if (phase_ == Phase::Draining) {
            ConversationPlayoutDrain::Snapshot snapshot{
                current.connection_epoch, current.response_generation, current.reset_epoch,
                drain_.DrainId(), audio->stopped, audio->decode_queue_size,
                audio->playback_queue_size, audio->decode_in_flight,
                audio->output_in_flight, drained};
            switch (drain_.Poll(now_us, snapshot)) {
                case ConversationPlayoutDrain::Action::Cancelled:
                    return Finish(EffectKind::Cancel);
                case ConversationPlayoutDrain::Action::TimedOut:
                    return Finish(EffectKind::Recover);
                case ConversationPlayoutDrain::Action::Complete:
                    phase_ = Phase::Ack;
                    break;
                case ConversationPlayoutDrain::Action::None:
                    return {};
            }
        }
        if (!quiescent || !drained) return {};
        if (delivery_ready_ && delivery_ == Delivery::Sent) {
            return Finish(EffectKind::Complete);
        }
        if (request_pending_) return {};
        ++request_id_;
        request_pending_ = true;
        return MakeEffect(EffectKind::SubmitAck);
    }

private:
    enum class Phase { Idle, Draining, Ack, Terminal };

    bool Active() const { return phase_ == Phase::Draining || phase_ == Phase::Ack; }
    static bool Matches(const Token& token, const Ownership& current) {
        return !current.stopped && token.connection_epoch == current.connection_epoch &&
            token.response_generation == current.response_generation &&
            token.reset_epoch == current.reset_epoch;
    }
    bool SameToken(const Token& token) const {
        return Matches(token, owner_) && token.drain_id == drain_.DrainId();
    }
    Token CurrentToken() const {
        return {owner_.connection_epoch, owner_.response_generation,
                owner_.reset_epoch, drain_.DrainId()};
    }
    Effect MakeEffect(EffectKind kind) const {
        Effect effect;
        effect.kind = kind;
        effect.request_id = request_id_;
        effect.connection_epoch = owner_.connection_epoch;
        effect.response_generation = owner_.response_generation;
        effect.reset_epoch = owner_.reset_epoch;
        effect.drain_id_size = drain_.DrainId().size();
        drain_.DrainId().copy(effect.drain_id.data(), effect.drain_id_size);
        return effect;
    }
    Effect Finish(EffectKind kind) {
        auto effect = MakeEffect(kind);
        phase_ = Phase::Terminal;
        request_pending_ = false;
        delivery_ready_ = false;
        return effect;
    }

    ConversationPlayoutDrain drain_;
    Ownership owner_;
    Phase phase_ = Phase::Idle;
    uint64_t started_us_ = 0;
    uint64_t request_id_ = 0;
    uint32_t codec_epoch_ = 0;
    bool codec_latched_ = false;
    bool cancel_requested_ = false;
    bool request_pending_ = false;
    bool delivery_ready_ = false;
    Delivery delivery_ = Delivery::Busy;
};
