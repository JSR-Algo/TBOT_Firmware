#pragma once
#include "protocols/connection_source.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <cstring>

// One app response publisher; one serialized WebSocket OnData stop publisher.
// Embedded in per-protocol signals: callbacks from replaced protocols never
// share these cells. Error/Closed/app code must not publish stop/fault markers.
class ChatPlayoutIntake {
public:
    struct Response {
        ConnectionSource source;
        uint64_t protocol_generation=0;
        uint32_t connect_generation=0,response_generation=0,reset_token=0;
    };
    struct Capture { Response response; uint32_t stamp=0; };
    struct Stop {
        Capture capture;
        uint64_t received_us=0,reset_epoch=0;
        std::array<char,129> drain_id{};
        size_t drain_id_size=0;
        bool reset_captured=false,valid=false,interrupt=false,conflict=false;
        bool continue_listening=false,realtime=false,explicit_manual_stop=false;
    };
    enum class Read { None, Ready, Fault };
    uint32_t Establish(const Response& response) {
        const auto prior=sequence_.load(std::memory_order_seq_cst);
        if (prior>=UINT32_MAX-1) { sequence_.store(UINT32_MAX,std::memory_order_seq_cst);return 0; }
        sequence_.store(prior+1,std::memory_order_seq_cst);
        source_.store(response.source.source_id,std::memory_order_seq_cst);
        connection_.store(response.source.connection_epoch,std::memory_order_seq_cst);
        protocol_low_.store(static_cast<uint32_t>(response.protocol_generation),std::memory_order_seq_cst);
        protocol_high_.store(static_cast<uint32_t>(response.protocol_generation>>32),std::memory_order_seq_cst);
        connect_.store(response.connect_generation,std::memory_order_seq_cst);
        response_.store(response.response_generation,std::memory_order_seq_cst);
        reset_.store(response.reset_token,std::memory_order_seq_cst);
        sequence_.store(prior+2,std::memory_order_seq_cst);
        return prior+2;
    }
    bool Current(uint32_t stamp) const {
        return stamp && !(stamp&1U) && sequence_.load(std::memory_order_seq_cst)==stamp;
    }
    bool TryCapture(Capture& out) const {
        Capture value;
        value.stamp=sequence_.load(std::memory_order_seq_cst);
        if (!value.stamp || (value.stamp&1U)) return false;
        value.response.source={source_.load(std::memory_order_seq_cst),connection_.load(std::memory_order_seq_cst)};
        value.response.protocol_generation=protocol_low_.load(std::memory_order_seq_cst);
        value.response.protocol_generation|=uint64_t(protocol_high_.load(std::memory_order_seq_cst))<<32;
        value.response.connect_generation=connect_.load(std::memory_order_seq_cst);
        value.response.response_generation=response_.load(std::memory_order_seq_cst);
        value.response.reset_token=reset_.load(std::memory_order_seq_cst);
        if (!Current(value.stamp)) return false;
        out=value;return true;
    }
    bool PublishStop(const Stop& stop) {
        const auto stamp=stop.capture.stamp;
        if (!Current(stamp)) return false;
        if (fault_stamp_.load(std::memory_order_acquire)==stamp) return false;
        if ((!stop.valid || !stop.reset_captured || stop.drain_id_size>128) && !stop.interrupt) {
            fault_stamp_.store(stamp,std::memory_order_release);return false;
        }
        writing_stamp_.store(stamp,std::memory_order_release);
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);
        if (!lock.owns_lock()) { fault_stamp_.store(stamp,std::memory_order_release);return false; }
        if (!Current(stamp)) return false;
        if (published_stamp_.load(std::memory_order_acquire)==stamp) {
            if (stop_.interrupt || stop_.conflict) return false;
            if (stop.interrupt || stop.drain_id_size!=stop_.drain_id_size || stop.reset_epoch!=stop_.reset_epoch ||
                stop.continue_listening!=stop_.continue_listening || stop.realtime!=stop_.realtime ||
                stop.explicit_manual_stop!=stop_.explicit_manual_stop ||
                std::memcmp(stop.drain_id.data(),stop_.drain_id.data(),stop.drain_id_size)!=0) {
                const auto original=stop_.received_us;
                stop_=stop;stop_.received_us=original;stop_.conflict=!stop.interrupt;
                return false;
            }
            return true;
        }
        stop_=stop;
        published_stamp_.store(stamp,std::memory_order_release);
        return true;
    }
    Read TryCollect(uint32_t stamp,Stop& out) {
        if (!Current(stamp)) return Read::None;
        if (fault_stamp_.load(std::memory_order_acquire)==stamp)
            return Current(stamp) ? Read::Fault : Read::None;
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);
        // A terminal observation cannot hide behind this lock until after its
        // deadline. Conservatively recover if the retained original is unreadable.
        if (!lock.owns_lock()) {
            const bool current_work = writing_stamp_.load(std::memory_order_acquire)==stamp ||
                published_stamp_.load(std::memory_order_acquire)==stamp;
            return Current(stamp) && current_work ? Read::Fault : Read::None;
        }
        if (published_stamp_.load(std::memory_order_acquire)!=stamp) return Read::None;
        if (!Current(stamp) || stop_.capture.stamp!=stamp) return Read::None;
        out=stop_;return Read::Ready;
    }
private:
    std::atomic<uint32_t> sequence_{0},source_{0},connection_{0},protocol_low_{0},protocol_high_{0};
    std::atomic<uint32_t> connect_{0},response_{0},reset_{0};
    std::atomic<uint32_t> published_stamp_{0},fault_stamp_{0},writing_stamp_{0};
    std::mutex mutex_;
    Stop stop_;
};
