#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "itmo_notification/due_notification.hpp"
#include "itmo_notification/notification.hpp"

namespace itmo_notification {

// In-memory планировщик уведомлений. API менять нельзя: он соответствует
// HTTP-ручкам.
class NotificationService {
public:
    NotificationService();
    ~NotificationService();

    NotificationService(const NotificationService&) = delete;
    NotificationService& operator=(const NotificationService&) = delete;

    // POST /v1/notifications
    void add(Notification notification);

    // DELETE /v1/notifications/{id}
    bool cancel(std::string_view id);

    // POST /v1/notifications/{id}/sent
    bool markSent(std::string_view id);

    // GET /v1/notifications/{id}
    std::optional<Notification> get(std::string_view id) const;

    // GET /v1/due?now=...&limit=...
    std::vector<DueNotification> due(std::int64_t now, std::size_t limit) const;

private:
    struct DueIndexEntry {
        std::int64_t send_at{};
        int priority{};
        std::int64_t created_at{};
        std::string id;
    };

    struct DueIndexEntryLess {
        bool operator()(const DueIndexEntry& lhs,
                        const DueIndexEntry& rhs) const;
    };

    using DueIndex = std::set<DueIndexEntry, DueIndexEntryLess>;

    struct NotificationRecord {
        Notification notification;
        DueIndex::iterator due_index_it{};
        bool in_due_index{false};
    };

    static DueIndexEntry MakeDueIndexEntry(const Notification& notification);

    void IndexPending(NotificationRecord& record);
    void UnindexIfPresent(NotificationRecord& record);

    std::unordered_map<std::string, NotificationRecord> notifications_;
    DueIndex due_index_;

    mutable std::shared_mutex mu_;
};

}  // namespace itmo_notification
