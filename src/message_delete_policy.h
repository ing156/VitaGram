#pragma once

namespace message_delete {
struct Permission {
    bool allowed = false;
    bool revoke = false;
};

// Local deletion is offered only in the account's own Saved Messages.
// Other chats retain the existing delete-for-everyone action.
inline Permission permission(bool saved_messages, bool for_everyone, bool only_for_self) {
    return {for_everyone || (saved_messages && only_for_self), for_everyone};
}
} // namespace message_delete
