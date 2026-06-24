#include "itmo_notification/notification_service.hpp"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace itmo_notification {
namespace {

DueNotification ToDue(const Notification& notification) {
    return {
        notification.id,
        notification.user_id,
        notification.channel,
        notification.recipient,
        notification.template_name,
        notification.payload,
        notification.send_at,
        notification.priority,
        notification.created_at,
    };
}

}  // namespace

NotificationService::NotificationService() = default;
NotificationService::~NotificationService() = default;

bool NotificationService::DueIndexEntryLess::operator()(
    const DueIndexEntry& lhs, const DueIndexEntry& rhs) const {
    if (lhs.send_at != rhs.send_at) {
        return lhs.send_at < rhs.send_at;
    }
    if (lhs.priority != rhs.priority) {
        return lhs.priority > rhs.priority;
    }
    if (lhs.created_at != rhs.created_at) {
        return lhs.created_at < rhs.created_at;
    }
    return lhs.id < rhs.id;
}

NotificationService::DueIndexEntry NotificationService::MakeDueIndexEntry(
    const Notification& notification) {
    return {
        notification.send_at,
        notification.priority,
        notification.created_at,
        notification.id,
    };
}

void NotificationService::IndexPending(NotificationRecord& record) {
    auto [due_it, inserted] = due_index_.insert(
        MakeDueIndexEntry(record.notification));
    (void)inserted;

    record.due_index_it = due_it;
    record.in_due_index = true;
}

void NotificationService::UnindexIfPresent(NotificationRecord& record) {
    if (!record.in_due_index) {
        return;
    }

    due_index_.erase(record.due_index_it);
    record.due_index_it = due_index_.end();
    record.in_due_index = false;
}

void NotificationService::add(Notification notification) {
    std::unique_lock<std::shared_mutex> lock(mu_);

    const std::string key = notification.id;
    auto it = notifications_.find(key);
    if (it != notifications_.end() &&
        it->second.notification.status == NotificationStatus::Pending) {
        return;
    }

    notification.status = NotificationStatus::Pending;
    if (it == notifications_.end()) {
        NotificationRecord record;
        record.notification = std::move(notification);

        auto [record_it, inserted] = notifications_.emplace(key,
                                                            std::move(record));
        (void)inserted;
        IndexPending(record_it->second);
        return;
    }

    UnindexIfPresent(it->second);
    it->second.notification = std::move(notification);
    IndexPending(it->second);
}

bool NotificationService::cancel(std::string_view id) {
    std::unique_lock<std::shared_mutex> lock(mu_);

    auto it = notifications_.find(std::string(id));
    if (it == notifications_.end()) {
        return false;
    }

    auto& record = it->second;
    if (record.notification.status == NotificationStatus::Cancelled) {
        return true;
    }
    if (record.notification.status == NotificationStatus::Sent) {
        return false;
    }

    UnindexIfPresent(record);
    record.notification.status = NotificationStatus::Cancelled;
    return true;
}

bool NotificationService::markSent(std::string_view id) {
    std::unique_lock<std::shared_mutex> lock(mu_);

    auto it = notifications_.find(std::string(id));
    if (it == notifications_.end()) {
        return false;
    }

    auto& record = it->second;
    if (record.notification.status == NotificationStatus::Sent) {
        return true;
    }
    if (record.notification.status == NotificationStatus::Cancelled) {
        return false;
    }

    UnindexIfPresent(record);
    record.notification.status = NotificationStatus::Sent;
    return true;
}

std::optional<Notification> NotificationService::get(std::string_view id) const {
    std::shared_lock<std::shared_mutex> lock(mu_);

    auto it = notifications_.find(std::string(id));
    if (it == notifications_.end()) {
        return std::nullopt;
    }
    return it->second.notification;
}

std::vector<DueNotification> NotificationService::due(
    std::int64_t now, std::size_t limit) const {
    if (limit == 0) {
        return {};
    }

    std::shared_lock<std::shared_mutex> lock(mu_);

    std::vector<DueNotification> result;
    result.reserve(std::min(limit, due_index_.size()));
    for (auto it = due_index_.begin(); it != due_index_.end() &&
        result.size() < limit; ++it) {
        if (it->send_at > now) {
            break;
        }

        auto notification_it = notifications_.find(it->id);
        if (notification_it == notifications_.end()) {
            continue;
        }

        const auto& notification = notification_it->second.notification;
        if (notification.status != NotificationStatus::Pending) {
            continue;
        }
        result.push_back(ToDue(notification));
    }
    return result;
}

}  // namespace itmo_notification
