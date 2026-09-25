#include "chat_connection_messages.h"
#include <cassert>
#include <string>

int main() {
    ChatConnectionMessages messages;
    ChatConnectionMessages::Owner owner{{1,7},1,1};
    const std::string text(65535,'x');
    auto first=messages.Admit(owner,text,100);
    assert(first && messages.Size()==1);
    assert(*messages.Front()->payload==text && messages.Front()->deadline_us==10000100);
    assert(messages.Admit(owner,"{\"escaped\":\"a\\nb\"}",101));
    assert(!messages.Admit(owner,"third",102));
    auto* active=messages.Front();
    active->physical.request_id=1;active->physical.generation=1;active->reservation=11;
    active->submitted=true;
    messages.ObserveRetirement(11);
    assert(!active->submitted && active->id==first && active->deadline_us==10000100);
    active->physical.request_id=2;active->physical.generation=2;active->reservation=12;active->submitted=true;
    ChatOutboundMailbox::Completion completion;
    completion.job=active->physical;completion.result=ChatOutboundMailbox::Result::Sent;completion.stale=true;
    assert(messages.Deliver(completion));
    assert(messages.Front()->outcome==ChatConnectionMessages::Outcome::Sent);
    messages.Pop();
    assert(!messages.Admit(owner,std::string(65536,'x'),100));
    messages.Cancel({{2,7},1,1});
    assert(messages.Front()->outcome==ChatConnectionMessages::Outcome::Cancelled);
}
