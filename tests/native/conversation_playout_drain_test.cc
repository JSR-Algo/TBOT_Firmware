#include "audio/conversation_playout_drain.h"

#include <cassert>
#include <limits>
#include <string>

using Drain = ConversationPlayoutDrain;
using Action = Drain::Action;
using State = Drain::State;

static Drain::Token Token(std::string_view id = "chat:1") {
    return {1, 2, 3, id};
}

static Drain::Snapshot Drained(std::string_view id = "chat:1") {
    Drain::Snapshot snapshot{};
    snapshot.connection_epoch = 1;
    snapshot.response_generation = 2;
    snapshot.reset_epoch = 3;
    snapshot.drain_id = id;
    snapshot.hardware_drained = true;
    return snapshot;
}

static void TestIdleAndCompletionOnce() {
    Drain drain;
    assert(drain.GetState() == State::Idle);
    assert(drain.Poll(0, Drained()) == Action::None);
    assert(drain.Start(0, Token()));
    assert(drain.GetState() == State::Pending);
    assert(drain.DrainId() == "chat:1");
    assert(drain.Poll(0, Drained()) == Action::Complete);
    assert(drain.GetState() == State::Complete);
    assert(drain.Poll(1, Drained()) == Action::None);
    drain.Cancel();
    assert(drain.Poll(2, Drained()) == Action::None);
}

static void TestEveryPendingStage() {
    for (int stage = 0; stage != 5; ++stage) {
        Drain drain;
        assert(drain.Start(0, Token()));
        auto snapshot = Drained();
        if (stage == 0) snapshot.decode_queue_count = 1;
        if (stage == 1) snapshot.playback_queue_count = 1;
        if (stage == 2) snapshot.decode_in_flight = true;
        if (stage == 3) snapshot.output_in_flight = true;
        if (stage == 4) snapshot.hardware_drained = false;
        assert(drain.Poll(9'999'999, snapshot) == Action::None);
        assert(drain.GetState() == State::Pending);
        assert(drain.Poll(9'999'999, Drained()) == Action::Complete);
    }
}

static void TestTimeoutBoundaryAndLateDrain() {
    for (uint64_t now : {10'000'000ULL, 10'000'001ULL}) {
        Drain drain;
        assert(drain.Start(0, Token()));
        assert(drain.Poll(now, Drained()) == Action::TimedOut);
        assert(drain.GetState() == State::TimedOut);
        assert(drain.Poll(now + 1, Drained()) == Action::None);
    }
    Drain drain;
    assert(drain.Start(42, Token()));
    auto snapshot = Drained();
    snapshot.hardware_drained = false;
    assert(drain.Poll(10'000'041, snapshot) == Action::None);
    assert(drain.Poll(10'000'042, snapshot) == Action::TimedOut);
}

static void TestCancellationAndEpochFences() {
    for (int cause = 0; cause != 5; ++cause) {
        Drain drain;
        assert(drain.Start(0, Token()));
        auto snapshot = Drained();
        if (cause == 0) ++snapshot.connection_epoch;
        if (cause == 1) ++snapshot.response_generation;
        if (cause == 2) ++snapshot.reset_epoch;
        if (cause == 3) snapshot.stopped = true;
        if (cause == 4) drain.Cancel();
        assert(drain.Poll(1, snapshot) == Action::Cancelled);
        assert(drain.GetState() == State::Cancelled);
        assert(drain.Poll(2, Drained()) == Action::None);
    }
    Drain idle;
    idle.Cancel();
    assert(idle.Poll(0, Drained()) == Action::None);
}

static void TestDuplicateAndSupersession() {
    Drain drain;
    assert(drain.Start(0, Token()));
    assert(drain.Start(9'000'000, Token()));
    assert(drain.Poll(10'000'000, Drained()) == Action::TimedOut);
    assert(drain.Start(11'000'000, Token("chat:2")));
    assert(drain.DrainId() == "chat:2");
    assert(drain.Poll(11'000'000, Drained("chat:2")) == Action::Complete);

    assert(drain.Start(0, Token()));
    auto replacement = Token("chat:3");
    ++replacement.response_generation;
    assert(drain.Start(8'000'000, replacement));
    assert(drain.Poll(8'000'001, Drained()) == Action::Cancelled);
    assert(drain.DrainId() == "chat:3");

    assert(drain.Start(0, Token()));
    assert(drain.Start(9'000'000, Token("chat:4")));
    assert(drain.Poll(10'000'000, Drained("chat:4")) == Action::Complete);
    assert(drain.Start(0, Token()));
    assert(drain.Start(1, Token("chat:5")));
    assert(drain.Poll(2, Drained()) == Action::Cancelled);
}

static void TestValidationAndOwnedId() {
    Drain drain;
    for (const auto id : {"", "chat", "other:1", "chat:"}) {
        assert(!drain.Start(0, Token(id)));
        assert(drain.GetState() == State::Idle);
    }
    std::string id = "chat:" + std::string(123, 'x');
    assert(id.size() == 128);
    assert(drain.Start(0, Token(id)));
    const auto accepted = id;
    id[5] = 'y';
    assert(drain.DrainId() == accepted);
    assert(!drain.Start(9'000'000, Token(id + 'x')));
    assert(drain.DrainId() == accepted);
    assert(drain.Poll(10'000'000, Drained(accepted)) == Action::TimedOut);
    const std::string embedded_null("chat:a\0b", 8);
    assert(!drain.Start(0, Token(embedded_null)));
}

static void TestClockRange() {
    Drain drain;
    const uint64_t start = std::numeric_limits<uint64_t>::max() - 100;
    assert(drain.Start(start, Token()));
    assert(drain.Poll(start + 1, Drained()) == Action::Complete);
}

static void TestTerminalDuplicatesStayTerminal() {
    for (int terminal = 0; terminal != 3; ++terminal) {
        Drain drain;
        assert(drain.Start(0, Token()));
        if (terminal == 1) drain.Cancel();
        const uint64_t now = terminal == 2 ? Drain::kTimeoutUs : 1;
        const Action expected = terminal == 0 ? Action::Complete :
            terminal == 1 ? Action::Cancelled : Action::TimedOut;
        assert(drain.Poll(now, Drained()) == expected);
        const auto state = drain.GetState();
        assert(drain.Start(now + 1, Token()));
        assert(drain.GetState() == state);
        assert(drain.Poll(now + 2, Drained()) == Action::None);
    }
}

int main() {
    TestIdleAndCompletionOnce();
    TestEveryPendingStage();
    TestTimeoutBoundaryAndLateDrain();
    TestCancellationAndEpochFences();
    TestDuplicateAndSupersession();
    TestValidationAndOwnedId();
    TestClockRange();
    TestTerminalDuplicatesStayTerminal();
}
