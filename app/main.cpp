#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "itmo_notification/due_notification.hpp"
#include "itmo_notification/notification.hpp"
#include "itmo_notification/notification_service.hpp"

namespace {

constexpr const char* kDefaultHost = "0.0.0.0";
constexpr int kDefaultPort = 8080;

using json = nlohmann::json;

std::int64_t unixNow() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string statusToString(itmo_notification::NotificationStatus status) {
    switch (status) {
        case itmo_notification::NotificationStatus::Pending:
            return "pending";
        case itmo_notification::NotificationStatus::Sent:
            return "sent";
        case itmo_notification::NotificationStatus::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

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
    n.created_at = j.value("created_at", unixNow());
    return n;
}

json notificationToJson(const itmo_notification::Notification& n) {
    return {
        {"id", n.id},
        {"user_id", n.user_id},
        {"channel", n.channel},
        {"recipient", n.recipient},
        {"template", n.template_name},
        {"payload", json::parse(n.payload, nullptr, false)},
        {"send_at", n.send_at},
        {"priority", n.priority},
        {"created_at", n.created_at},
        {"status", statusToString(n.status)},
    };
}

json dueToJson(const itmo_notification::DueNotification& n) {
    return {
        {"id", n.id},
        {"user_id", n.user_id},
        {"channel", n.channel},
        {"recipient", n.recipient},
        {"template", n.template_name},
        {"payload", json::parse(n.payload, nullptr, false)},
        {"send_at", n.send_at},
        {"priority", n.priority},
        {"created_at", n.created_at},
    };
}

void writeJson(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
    itmo_notification::NotificationService service;
    httplib::Server server;

    server.Get(
        "/health",
        [](const httplib::Request& /*req*/, httplib::Response& res) {
            writeJson(res, 200, {{"ok", true}});
        });

    server.Post(
        "/v1/notifications",
        [&](const httplib::Request& req, httplib::Response& res) {
            try {
                auto notification = parseNotification(json::parse(req.body));
                if (notification.id.empty() || notification.user_id.empty() ||
                    notification.channel.empty()) {
                    writeJson(
                        res, 400,
                        {{"error", "id, user_id and channel are required"}});
                    return;
                }
                service.add(std::move(notification));
                writeJson(res, 201, {{"ok", true}});
            } catch (const std::exception& e) {
                writeJson(
                    res, 400,
                    {{"error", "bad json"}, {"detail", e.what()}});
            }
        });

    server.Get(
        R"(/v1/notifications/([^/]+))",
        [&](const httplib::Request& req, httplib::Response& res) {
            const std::string id = req.matches[1].str();
            const auto n = service.get(id);
            if (!n.has_value()) {
                writeJson(res, 404, {{"error", "notification not found"}});
                return;
            }
            writeJson(res, 200, notificationToJson(*n));
        });

    server.Delete(
        R"(/v1/notifications/([^/]+))",
        [&](const httplib::Request& req, httplib::Response& res) {
            const std::string id = req.matches[1].str();
            if (!service.cancel(id)) {
                writeJson(res, 404, {{"error", "notification not found"}});
                return;
            }
            res.status = 204;
        });

    server.Post(
        R"(/v1/notifications/([^/]+)/sent)",
        [&](const httplib::Request& req, httplib::Response& res) {
            const std::string id = req.matches[1].str();
            if (!service.markSent(id)) {
                writeJson(
                    res, 404,
                    {{"error", "notification not found or cancelled"}});
                return;
            }
            writeJson(res, 200, {{"ok", true}});
        });

    server.Get(
        "/v1/due",
        [&](const httplib::Request& req, httplib::Response& res) {
            std::int64_t now = unixNow();
            std::size_t limit = 100;
            try {
                if (req.has_param("now")) {
                    now = static_cast<std::int64_t>(
                        std::stoll(req.get_param_value("now")));
                }
                if (req.has_param("limit")) {
                    limit = static_cast<std::size_t>(
                        std::stoul(req.get_param_value("limit")));
                }
            } catch (const std::exception&) {
                writeJson(res, 400, {{"error", "now/limit must be integers"}});
                return;
            }

            const auto due = service.due(now, limit);
            json body = json::array();
            for (const auto& n : due) {
                body.push_back(dueToJson(n));
            }
            writeJson(res, 200, body);
        });

    std::cout << "notification_service listening on " << kDefaultHost << ":"
        << kDefaultPort << "\n";
    if (!server.listen(kDefaultHost, kDefaultPort)) {
        std::cerr << "failed to bind " << kDefaultHost << ":" << kDefaultPort
            << "\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
