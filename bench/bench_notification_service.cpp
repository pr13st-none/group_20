#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>
#include <nlohmann/json.hpp>

#include "itmo_notification/notification.hpp"
#include "itmo_notification/notification_service.hpp"

namespace {

using json = nlohmann::json;

const char* dataPath() {
    if (const char* p = std::getenv("ITMO_NOTIFICATION_DATA")) {
        return p;
    }
    return ITMO_NOTIFICATION_DEFAULT_DATA;
}

const char* metaPath() {
    if (const char* p = std::getenv("ITMO_NOTIFICATION_META")) {
        return p;
    }
    return ITMO_NOTIFICATION_DEFAULT_META;
}

struct Dataset {
    std::vector<itmo_notification::Notification> notifications;
    std::vector<std::string> sample_ids;
    std::int64_t due_now{};
};

itmo_notification::Notification parseNotification(const json& j) {
    itmo_notification::Notification n;
    n.id = j.value("id", std::string{});
    n.user_id = j.value("user_id", std::string{});
    n.channel = j.value("channel", std::string{});
    n.recipient = j.value("recipient", std::string{});
    n.template_name = j.value("template", std::string{});
    n.payload = j.contains("payload") ? j.at("payload").dump() : "{}";
    n.send_at = j.value("send_at", std::int64_t{0});
    n.priority = j.value("priority", 0);
    n.created_at = j.value("created_at", std::int64_t{0});
    return n;
}

const Dataset& dataset() {
    static const Dataset ds = []() {
        Dataset d;
        std::ifstream in(dataPath());
        if (!in) {
            std::cerr << "FATAL: cannot open dataset " << dataPath()
                << " — run scripts/gen_dataset.py first\n";
            std::exit(1);
        }

        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) {
                continue;
            }
            d.notifications.push_back(parseNotification(json::parse(line)));
        }

        std::ifstream min(metaPath());
        if (!min) {
            std::cerr << "FATAL: cannot open meta " << metaPath() << "\n";
            std::exit(1);
        }

        const auto meta = json::parse(min);
        d.sample_ids = meta.at("sample_ids").get<std::vector<std::string>>();
        d.due_now = meta.at("due_now").get<std::int64_t>();
        return d;
    }();
    return ds;
}

const itmo_notification::NotificationService& warmService() {
    static const itmo_notification::NotificationService& service =
        []() -> const itmo_notification::NotificationService& {
            static itmo_notification::NotificationService s;
            for (const auto& n : dataset().notifications) {
                s.add(n);
            }
            return s;
        }();
    return service;
}

std::unique_ptr<itmo_notification::NotificationService> warmMutableService() {
    auto service = std::make_unique<itmo_notification::NotificationService>();
    for (const auto& n : dataset().notifications) {
        service->add(n);
    }
    return service;
}

}  // namespace

static void BM_Due_SmallLimit(benchmark::State& state) {
    const auto& service = warmService();
    for (auto _ : state) {
        auto due = service.due(dataset().due_now, 100);
        benchmark::DoNotOptimize(due);
    }
}
BENCHMARK(BM_Due_SmallLimit)->Unit(benchmark::kMicrosecond);

static void BM_Due_NoReady(benchmark::State& state) {
    const auto& service = warmService();
    for (auto _ : state) {
        auto due = service.due(1'600'000'000, 100);
        benchmark::DoNotOptimize(due);
    }
}
BENCHMARK(BM_Due_NoReady)->Unit(benchmark::kMicrosecond);

static void BM_Add_Throughput(benchmark::State& state) {
    itmo_notification::NotificationService service;
    const auto& d = dataset();
    std::size_t i = 0;
    for (auto _ : state) {
        auto n = d.notifications[i % d.notifications.size()];
        n.id = "bench-" + std::to_string(i);
        service.add(std::move(n));
        ++i;
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_Add_Throughput)->Unit(benchmark::kMicrosecond);

static void BM_MarkSent(benchmark::State& state) {
    auto service = warmMutableService();
    const auto& ids = dataset().sample_ids;
    std::size_t i = 0;
    for (auto _ : state) {
        const auto& id = ids[i % ids.size()];
        auto ok = service->markSent(id);
        benchmark::DoNotOptimize(ok);
        ++i;
    }
}
BENCHMARK(BM_MarkSent)->Unit(benchmark::kMicrosecond);

static void BM_CancelledDoNotPoisonDue(benchmark::State& state) {
    auto service = warmMutableService();
    const auto& d = dataset();
    for (std::size_t i = 0; i < d.sample_ids.size(); ++i) {
        service->cancel(d.sample_ids[i]);
    }

    for (auto _ : state) {
        auto due = service->due(d.due_now, 100);
        benchmark::DoNotOptimize(due);
    }
}
BENCHMARK(BM_CancelledDoNotPoisonDue)->Unit(benchmark::kMicrosecond);
