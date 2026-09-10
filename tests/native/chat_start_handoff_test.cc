#include <cassert>
#include <atomic>
#include <thread>
#define private public
#include "chat_start_handoff.h"
#undef private

int main() {
    {
        ChatStartHandoff opening;
        ChatStartHandoff::Request early{{1,2},1,1,0,200000,250000};
        assert(opening.Publish(early));ChatStartHandoff::Request captured;
        assert(opening.TryRequest(captured) && captured.received_us==200000);
        assert(captured.admission_deadline_us==250000);
        assert(opening.Admit(captured,1,1,249999));
        assert(!opening.Confirm(early,250000));
    }
    ChatStartHandoff handoff;
    ChatStartHandoff::Request request{{4, 8}, 0x1234567887654321ULL, 3, 0, 1000};
    assert(handoff.Publish(request));
    assert(request.serial == 1);
    ChatStartHandoff::Request captured;
    assert(handoff.TryRequest(captured));
    assert(captured.protocol_generation == request.protocol_generation);
    assert(captured.received_us == 1000);
    ChatStartHandoff::Admission admission;
    assert(!handoff.TryAdmission(request, 1001, admission));
    assert(handoff.Admit(captured, 7, 9, 1001));
    assert(handoff.TryAdmission(request, 250999, admission));
    assert(admission.response_generation == 7 && admission.reset_token == 9);
    assert(!handoff.Admit(captured, 18, 19, 1002));
    assert(handoff.Confirm(request, 1002));
    assert(handoff.Confirmed(request));
    assert(!handoff.TryAdmission(request, 251000, admission));
    handoff.Expire(request);
    assert(!handoff.Admit(captured, 7, 10, 1002));
    auto replacement = request;
    replacement.received_us = 500000;
    assert(handoff.Publish(replacement));
    assert(replacement.serial == 2);
    handoff.Expire(request);
    assert(!handoff.Admit(request, 8, 11, 500001));
    auto stale = replacement;
    stale.source.source_id++;
    assert(!handoff.Admit(stale, 8, 11, 500001));
    assert(handoff.Admit(replacement, 8, 11, 500001));
    assert(handoff.TryAdmission(replacement, 500002, admission));
    assert(admission.response_generation == 8);
    ChatStartHandoff raced;
    auto racing = request;
    assert(raced.Publish(racing));
    std::thread receiver([&]{ raced.Expire(racing); });
    receiver.join();
    assert(!raced.Admit(racing, 1, 1, 1001));
    assert(!raced.TryAdmission(racing, 1001, admission));
    handoff.serial_ = UINT32_MAX - 1;
    assert(!handoff.Publish(replacement));
    assert(!handoff.TryAdmission(replacement, 500003, admission));
}
