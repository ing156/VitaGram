#include "message_delete_policy.h"
#include <cassert>
#include <cstdio>

int main() {
    // TDLib returns only-for-self for Saved Messages, including forwarded posts.
    auto saved = message_delete::permission(true, false, true);
    assert(saved.allowed && !saved.revoke);
    // Local rights in someone else's chat must not expose the delete-for-everyone button.
    auto other = message_delete::permission(false, false, true);
    assert(!other.allowed && !other.revoke);
    // Self-chat identity alone cannot substitute for Telegram's message rights.
    assert(!message_delete::permission(true, false, false).allowed);
    assert(!message_delete::permission(false, false, false).allowed);
    // Outgoing/group-admin deletion keeps revocation enabled.
    auto group = message_delete::permission(false, true, false);
    assert(group.allowed && group.revoke);
    auto private_chat = message_delete::permission(false, true, true);
    assert(private_chat.allowed && private_chat.revoke);
    std::puts("Message deletion: Saved Messages, denied local deletion elsewhere and revocation rights: PASS");
}
