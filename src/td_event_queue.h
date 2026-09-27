#pragma once
#include <deque>
#include <jansson.h>
#include <cstring>

// Caller holds the queue mutex. Only unsolicited file snapshots are replaceable.
// Move the newest snapshot to the end: never replay older progress after a newer
// message/terminal event. All request replies and message/auth/chat events survive.
inline void enqueue_td_event(std::deque<void*>& queue, json_t* root) {
    const char* type = json_string_value(json_object_get(root, "@type"));
    if (type && !std::strcmp(type, "updateFile") && !json_object_get(root, "@extra")) {
        json_int_t id = json_integer_value(json_object_get(json_object_get(root, "file"), "id"));
        if (id) for (auto it = queue.begin(); it != queue.end(); ++it) {
            auto* old = static_cast<json_t*>(*it);
            const char* old_type = json_string_value(json_object_get(old, "@type"));
            if (old_type && !std::strcmp(old_type, "updateFile") && !json_object_get(old, "@extra") &&
                json_integer_value(json_object_get(json_object_get(old, "file"), "id")) == id) {
                json_decref(old); queue.erase(it); break;
            }
        }
    }
    queue.push_back(root);
}
