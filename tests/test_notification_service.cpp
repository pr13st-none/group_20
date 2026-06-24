#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "itmo_notification/notification.hpp"
#include "itmo_notification/notification_service.hpp"

using itmo_notification::Notification;
using itmo_notification::NotificationService;
using itmo_notification::NotificationStatus;

namespace {

Notification makeNotification(
    std::string id, std::int64_t send_at, int priority = 0,
    std::int64_t created_at = 0) {
    Notification n;
    n.id = std::move(id);
    n.user_id = "u-1";
    n.channel = "email";
    n.recipient = "user@example.com";
    n.template_name = "payment_reminder";
    n.payload = R"({"order_id":"o-1"})";
    n.send_at = send_at;
    n.priority = priority;
    n.created_at = created_at;
    return n;
}

}  // namespace

// -----------------------------------------------------------------------------
// Тесты, которые должны проходить на baseline.
// -----------------------------------------------------------------------------

TEST(NotificationServiceTest, AddThenGetReturnsNotification) {
    NotificationService service;
    service.add(makeNotification("n1", 100));

    const auto n = service.get("n1");
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->id, "n1");
    EXPECT_EQ(n->status, NotificationStatus::Pending);
}

TEST(NotificationServiceTest, DueReturnsReadyNotification) {
    NotificationService service;
    service.add(makeNotification("n1", 100));

    const auto due = service.due(100, 10);
    ASSERT_EQ(due.size(), 1u);
    EXPECT_EQ(due[0].id, "n1");
}

TEST(NotificationServiceTest, FutureNotificationIsNotDue) {
    NotificationService service;
    service.add(makeNotification("n1", 200));

    EXPECT_TRUE(service.due(100, 10).empty());
}

TEST(NotificationServiceTest, CancelHidesNotificationFromDue) {
    NotificationService service;
    service.add(makeNotification("n1", 100));
    ASSERT_TRUE(service.cancel("n1"));

    EXPECT_TRUE(service.due(200, 10).empty());
    const auto n = service.get("n1");
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->status, NotificationStatus::Cancelled);
}

TEST(NotificationServiceTest, MarkSentHidesNotificationFromDue) {
    NotificationService service;
    service.add(makeNotification("n1", 100));
    ASSERT_TRUE(service.markSent("n1"));

    EXPECT_TRUE(service.due(200, 10).empty());
    const auto n = service.get("n1");
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->status, NotificationStatus::Sent);
}

TEST(NotificationServiceTest, LimitRestrictsDueResult) {
    NotificationService service;
    for (int i = 0; i < 5; ++i) {
        service.add(makeNotification("n" + std::to_string(i), 100 + i));
    }

    EXPECT_EQ(service.due(200, 2).size(), 2u);
}

TEST(NotificationServiceTest, SendAtBoundaryIsInclusive) {
    NotificationService service;
    service.add(makeNotification("n1", 100));

    EXPECT_EQ(service.due(99, 10).size(), 0u);
    EXPECT_EQ(service.due(100, 10).size(), 1u);
}

TEST(NotificationServiceTest, ConcurrentAddSentAndDueIsSafe) {
    NotificationService service;
    constexpr int kPerThread = 500;

    std::vector<std::thread> threads;
    threads.reserve(8);

    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                service.add(makeNotification(
                    "t" + std::to_string(t) + "-" + std::to_string(i),
                    100 + (i % 20), i % 5, i));
            }
        });
    }

    std::atomic<int> total_seen{0};
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < kPerThread; ++i) {
                const auto due = service.due(200, 25);
                total_seen.fetch_add(
                    static_cast<int>(due.size()), std::memory_order_relaxed);
            }
        });
    }
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                service.markSent(
                    "t" + std::to_string(t) + "-" + std::to_string(i));
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_GT(total_seen.load(), 0);
}

// -----------------------------------------------------------------------------
// Ранее отключённые тесты теперь включены и должны проходить.
// -----------------------------------------------------------------------------

TEST(NotificationServiceTest, DueOrderingUsesPriorityCreatedAtAndId) {
    NotificationService service;
    service.add(makeNotification("a-low", 100, 1, 10));
    service.add(makeNotification("z-high", 100, 9, 20));
    service.add(makeNotification("m-mid-old", 100, 5, 1));
    service.add(makeNotification("b-mid-new", 100, 5, 2));

    const auto due = service.due(100, 10);
    ASSERT_EQ(due.size(), 4u);
    EXPECT_EQ(due[0].id, "z-high");
    EXPECT_EQ(due[1].id, "m-mid-old");
    EXPECT_EQ(due[2].id, "b-mid-new");
    EXPECT_EQ(due[3].id, "a-low");
}

TEST(NotificationServiceTest, DuplicateIdDoesNotCreateDuplicateDueEntries) {
    NotificationService service;
    service.add(makeNotification("n1", 100, 1, 1));
    service.add(makeNotification("n1", 100, 9, 2));

    const auto due = service.due(100, 10);
    ASSERT_EQ(due.size(), 1u);
    EXPECT_EQ(due[0].id, "n1");
    EXPECT_EQ(due[0].priority, 1);
}

TEST(NotificationServiceTest, CancelledIdCanBeScheduledAgain) {
    NotificationService service;
    service.add(makeNotification("n1", 100));
    ASSERT_TRUE(service.cancel("n1"));

    service.add(makeNotification("n1", 200));

    const auto due = service.due(200, 10);
    ASSERT_EQ(due.size(), 1u);
    EXPECT_EQ(due[0].id, "n1");
}

TEST(NotificationServiceTest, SentIdCanBeScheduledAgain) {
    NotificationService service;
    service.add(makeNotification("n1", 100));
    ASSERT_TRUE(service.markSent("n1"));

    service.add(makeNotification("n1", 200));

    const auto due = service.due(200, 10);
    ASSERT_EQ(due.size(), 1u);
    EXPECT_EQ(due[0].id, "n1");
}

TEST(NotificationServiceTest, DueOrderingUsesCreatedAtBeforeId) {
    NotificationService service;
    service.add(makeNotification("z-old", 100, 5, 1));
    service.add(makeNotification("a-new", 100, 5, 2));

    const auto due = service.due(100, 10);
    ASSERT_EQ(due.size(), 2u);
    EXPECT_EQ(due[0].id, "z-old");
    EXPECT_EQ(due[1].id, "a-new");
}
