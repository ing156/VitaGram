#pragma once
#include "telegram_client.h"

// Album IDs identify a group, not a displayed row: videos in mixed albums stay
// separate. Only a photo row can disappear by merging with an older photo page.
struct HistorySelection {
    int64_t message_id = 0;
    int64_t photo_album_id = 0;

    static HistorySelection capture(const std::vector<MessageItem>& messages, int index) {
        if (index < 0 || index >= (int)messages.size()) return {};
        const auto& message = messages[index];
        return {message.id, message.photos.empty() ? 0 : message.media_album_id};
    }

    int find(const std::vector<MessageItem>& messages) const {
        if (!message_id) return -1;
        // An exact match always wins, even when an earlier row shares the album.
        for (size_t i = 0; i < messages.size(); ++i)
            if (messages[i].id == message_id) return (int)i;
        int merged = -1;
        if (photo_album_id) {
            for (size_t i = 0; i < messages.size(); ++i) {
                const auto& message = messages[i];
                if (!message.photos.empty() && message.media_album_id == photo_album_id &&
                    message.id <= message_id && (merged < 0 || message.id > messages[merged].id))
                    merged = (int)i;
            }
        }
        return merged;
    }
};
