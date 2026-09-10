#include "chat_outbound_mailbox.h"

#include <cassert>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <thread>

using Mailbox = ChatOutboundMailbox;
using Job = Mailbox::Job;
using Result = Mailbox::Result;

Job Make(uint64_t id, uint32_t generation,
         Mailbox::Kind kind = Mailbox::Kind::ListenStart) {
    Job job{};
    job.request_id = id;
    job.generation = generation;
    job.protocol_generation = (uint64_t{1} << 40) + 2;
    job.connection_epoch = 3;
    job.kind = kind;
    job.argument = 7;
    return job;
}

void Finish(Mailbox& mailbox, const Job& job, Result result = Result::Sent) {
    assert(mailbox.TryComplete(job, result));
    Mailbox::Completion completion{};
    assert(mailbox.TryCollect(completion));
    assert(completion.job.request_id == job.request_id);
    assert(completion.result == result);
    assert(!completion.stale);
}

void OrderingAndOwnership() {
    Mailbox mailbox;
    assert(!mailbox.IsCurrent(0));
    const auto generation = mailbox.AdvanceGeneration();
    assert(generation == 1);
    Job first = Make(1, generation);
    Job second = Make(2, generation, Mailbox::Kind::ListenStop);
    Job third = Make(3, generation, Mailbox::Kind::Abort);
    assert(mailbox.TrySubmit(first));
    assert(!mailbox.TrySubmit(first));
    assert(mailbox.TrySubmit(second));
    assert(!mailbox.TrySubmit(third));
    Job out = Make(999, 999);
    assert(mailbox.TryTake(out) && out.request_id == 1);
    assert(!mailbox.TrySubmit(first));
    assert(mailbox.TrySubmit(third));
    assert(!mailbox.TryTake(out) && out.request_id == 1);
    for (int field = 0; field < 7; ++field) {
        auto foreign = first;
        switch (field) {
            case 0: ++foreign.request_id; break;
            case 1: ++foreign.generation; break;
            case 2: ++foreign.protocol_generation; break;
            case 3: ++foreign.connection_epoch; break;
            case 4: foreign.kind = Mailbox::Kind::Wake; break;
            case 5: ++foreign.argument; break;
            case 6: assert(foreign.SetPayload("foreign", 7)); break;
        }
        assert(!mailbox.TryComplete(foreign, Result::Failed));
    }
    assert(mailbox.TryComplete(first, Result::Busy));
    assert(!mailbox.TryComplete(first, Result::Sent));
    assert(!mailbox.TrySubmit(first));
    assert(!mailbox.TryTake(out) && out.request_id == 1);
    Mailbox::Completion completion{};
    assert(mailbox.TryCollect(completion));
    assert(completion.result == Result::Busy && !completion.stale);
    assert(!mailbox.TryCollect(completion));
    assert(completion.job.request_id == 1 && completion.result == Result::Busy);
    assert(mailbox.TryTake(out) && out.kind == Mailbox::Kind::ListenStop);
    Finish(mailbox, out, Result::Failed);
    assert(mailbox.TryTake(out) && out.kind == Mailbox::Kind::Abort);
    Finish(mailbox, out, Result::Stale);
    assert(!mailbox.TryTake(out) && out.request_id == 3);
    assert(!mailbox.TryComplete(out, Result::Sent));
}

void Validation() {
    Mailbox mailbox;
    Job job = Make(1, mailbox.AdvanceGeneration(), Mailbox::Kind::DrainAck);
    for (int field = 0; field < 5; ++field) {
        auto invalid = job;
        switch (field) {
            case 0: invalid.request_id = 0; break;
            case 1: invalid.generation = 0; break;
            case 2: invalid.protocol_generation = 0; break;
            case 3: invalid.connection_epoch = 0; break;
            case 4: invalid.kind = static_cast<Mailbox::Kind>(99); break;
        }
        assert(!mailbox.TrySubmit(invalid));
    }
    char data[129];
    std::memset(data, 'x', sizeof(data));
    assert(job.SetPayload(data, 128));
    assert(job.payload_size == 128 && job.payload[128] == '\0');
    assert(!job.SetPayload(data, 129));
    assert(job.payload_size == 128);
    assert(!job.SetPayload("a\0b", 3));
    assert(!job.SetPayload(nullptr, 1));
    assert(mailbox.TrySubmit(job));
    data[0] = 'z';
    Job out{};
    assert(mailbox.TryTake(out) && out.payload[0] == 'x');
    Finish(mailbox, out);
    job.request_id = 2;
    job.payload_size = 129;
    assert(!mailbox.TrySubmit(job));
    job.payload_size = 128;
    job.payload[128] = 'x';
    assert(!mailbox.TrySubmit(job));
    job.payload[128] = '\0';
    job.payload[5] = '\0';
    assert(!mailbox.TrySubmit(job));
    assert(job.SetPayload(nullptr, 0));
    job.kind = Mailbox::Kind::Wake;
    assert(job.SetPayload("hello", 5));
    assert(mailbox.TrySubmit(job));
    assert(mailbox.TryTake(out));
    assert(std::strcmp(out.payload.data(), "hello") == 0);
    Finish(mailbox, out);
}

void CancellationBarrier() {
    Mailbox mailbox;
    auto generation = mailbox.AdvanceGeneration();
    assert(mailbox.TrySubmit(Make(1, generation)));
    assert(mailbox.TrySubmit(Make(2, generation)));
    std::mutex barrier;
    std::condition_variable cv;
    bool taken = false, release = false;
    std::thread worker([&] {
        Job active{};
        assert(mailbox.TryTake(active));
        {
            std::unique_lock<std::mutex> lock(barrier);
            taken = true;
            cv.notify_one();
            cv.wait(lock, [&] { return release; });
        }
        assert(mailbox.TryComplete(active, Result::Sent));
    });
    {
        std::unique_lock<std::mutex> lock(barrier);
        cv.wait(lock, [&] { return taken; });
    }
    generation = mailbox.AdvanceGeneration();
    assert(!mailbox.IsCurrent(1) && mailbox.IsCurrent(generation));
    // Active ownership survives cancellation, including duplicate request checks.
    assert(!mailbox.TrySubmit(Make(1, generation)));
    assert(mailbox.TrySubmit(Make(3, generation)));
    assert(mailbox.TrySubmit(Make(4, generation)));
    Job out = Make(999, 999);
    assert(!mailbox.TryTake(out) && out.request_id == 999);
    {
        std::lock_guard<std::mutex> lock(barrier);
        release = true;
    }
    cv.notify_one();
    worker.join();
    assert(!mailbox.TryTake(out) && out.request_id == 999);
    Mailbox::Completion completion{};
    assert(mailbox.TryCollect(completion));
    assert(completion.job.request_id == 1 && completion.stale);
    assert(completion.result == Result::Sent);
    assert(mailbox.TryTake(out) && out.request_id == 3);
    Finish(mailbox, out);
    assert(mailbox.TryTake(out) && out.request_id == 4);
    assert(mailbox.TryComplete(out, Result::Sent));
    mailbox.AdvanceGeneration();
    assert(mailbox.TryCollect(completion) && completion.stale);
    // Exercise stale skipping directly, without submit-side compaction.
    generation = mailbox.AdvanceGeneration();
    assert(mailbox.TrySubmit(Make(5, generation)));
    assert(mailbox.TrySubmit(Make(6, generation)));
    mailbox.AdvanceGeneration();
    assert(!mailbox.TryTake(out) && out.request_id == 4);
}

#ifdef MAILBOX_TEST_INSTRUMENTED
void ExhaustionAndContention() {
    Mailbox mailbox;
    auto generation = mailbox.AdvanceGeneration();
    auto job = Make(1, generation);
    assert(mailbox.TrySubmit(job));
    Job out = Make(999, 999);
    assert(mailbox.TryTake(out));
    std::mutex barrier;
    std::condition_variable cv;
    bool locked = false, release = false;
    std::thread holder([&] {
        std::lock_guard<std::mutex> mailbox_lock(mailbox.mutex_);
        std::unique_lock<std::mutex> lock(barrier);
        locked = true;
        cv.notify_one();
        cv.wait(lock, [&] { return release; });
    });
    {
        std::unique_lock<std::mutex> lock(barrier);
        cv.wait(lock, [&] { return locked; });
    }
    assert(!mailbox.TrySubmit(Make(2, generation)));
    assert(!mailbox.TryTake(out) && out.request_id == 1);
    assert(!mailbox.TryComplete(job, Result::Sent));
    Mailbox::Completion completion{};
    completion.job.request_id = 999;
    assert(!mailbox.TryCollect(completion) && completion.job.request_id == 999);
    assert(mailbox.AdvanceGeneration() == generation + 1);
    {
        std::lock_guard<std::mutex> lock(barrier);
        release = true;
    }
    cv.notify_one();
    holder.join();
    assert(mailbox.TryComplete(job, Result::Failed));
    assert(mailbox.TryCollect(completion) && completion.stale);
    constexpr auto exhausted = std::numeric_limits<uint32_t>::max();
    mailbox.generation_.store(exhausted - 2);
    assert(mailbox.AdvanceGeneration() == exhausted - 1);
    assert(mailbox.IsCurrent(exhausted - 1));
    assert(mailbox.TrySubmit(Make(3, exhausted - 1)));
    assert(mailbox.AdvanceGeneration() == exhausted);
    assert(mailbox.AdvanceGeneration() == exhausted);
    assert(!mailbox.IsCurrent(exhausted) && !mailbox.IsCurrent(exhausted - 1));
    assert(!mailbox.TrySubmit(Make(4, exhausted)));
    assert(!mailbox.TryTake(out));
}
#endif

int main() {
    OrderingAndOwnership();
    Validation();
    CancellationBarrier();
#ifdef MAILBOX_TEST_INSTRUMENTED
    ExhaustionAndContention();
#endif
}
