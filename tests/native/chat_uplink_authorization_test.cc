#include "audio/chat_uplink_authorization.h"
#include <cassert>

int main() {
    ChatUplinkAuthorization auth;
    const auto legacy = auth.Capture();
    assert(!legacy.chat_scope && auth.Accepts(legacy));
    assert(!auth.IsCurrentChat(legacy));
    const auto a = auth.Revoke();
    assert(a != 0 && !auth.Arm(a));
    auth.AcknowledgePrepared(a, true);
    assert(auth.Arm(a));
    const auto capture_a = auth.Capture();
    assert(capture_a.chat_scope && auth.IsCurrentChat(capture_a));
    assert(!auth.Accepts(legacy));
    const auto b = auth.Revoke();
    assert(!auth.Accepts(capture_a));
    assert(!auth.Arm(a) && !auth.Arm(b));
    auth.AcknowledgePrepared(a, true);
    assert(!auth.Arm(b));
    auth.AcknowledgePrepared(b, true);
    assert(auth.Arm(b));
    const auto capture_b = auth.Capture();
    assert(auth.IsCurrentChat(capture_b));
    assert(!auth.Accepts(capture_a));
    assert(!auth.Arm(b));
    const auto leave = auth.Revoke();
    auth.AcknowledgePrepared(leave, false);
    assert(!auth.Arm(leave, true));
    assert(auth.Arm(leave, false));
    const auto lesson = auth.Capture();
    assert(!lesson.chat_scope && lesson.generation != 0 && auth.Accepts(lesson));
    assert(!auth.Accepts(legacy) && !auth.Accepts(capture_b));
    const auto again = auth.Revoke();
    auth.AcknowledgePrepared(again, true);
    assert(auth.Arm(again));
    assert(!auth.Accepts(lesson));
}
