// XBlox MQTT blocks - mqttServer lifecycle + mqttClient publish/subscribe.

#include "blocks/mqtt_blocks.hpp"

#if defined(FEATURE_MQTT_XBLOX) && FEATURE_MQTT_XBLOX

#include "lib/net/mqtt_client.hpp"

#include <mqtt_server_cpp.hpp>

#include <boost/asio/ip/address.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "string/string_utils.hpp"

#endif

namespace media::xblox::blocks {
namespace {


#if defined(FEATURE_MQTT_XBLOX) && FEATURE_MQTT_XBLOX

using MqttServerEndpoint = MQTT_NS::server<>::endpoint_t;
using MqttServerEndpointPtr = std::shared_ptr<MqttServerEndpoint>;

struct MqttSubscription {
    MQTT_NS::buffer topic;
    MqttServerEndpointPtr endpoint;
    MQTT_NS::qos qos_value = MQTT_NS::qos::at_most_once;
};

class MqttServerInstance {
public:
    ~MqttServerInstance() { stop(); }

    bool start(std::string bind, int port, std::string& err)
    {
        if (running_.load()) {
            err = "MQTT server is already running";
            return false;
        }

        bind_ = bind.empty() ? "127.0.0.1" : std::move(bind);
        port_ = std::clamp(port, 1, 65535);
        error_.clear();
        accepts_.store(0);
        connects_.store(0);
        publishes_.store(0);
        subscriptions_count_.store(0);
        running_.store(true);

        thread_ = std::thread([this] {
            try {
                boost::asio::ip::tcp::endpoint endpoint(
                    boost::asio::ip::make_address(bind_),
                    static_cast<unsigned short>(port_));
                server_ = std::make_unique<MQTT_NS::server<>>(endpoint, ioc_);
                configure_server(*server_);
                server_->listen();
                ioc_.run();
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lk(mu_);
                error_ = e.what();
            }
            running_.store(false);
        });

        // Give bind/listen failures a short window to surface.
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::lock_guard<std::mutex> lk(mu_);
            if (!error_.empty()) {
                err = error_;
                stop();
                return false;
            }
        }
        return true;
    }

    void stop()
    {
        if (server_) {
            boost::system::error_code ec;
            boost::asio::post(ioc_, [this] {
                if (server_)
                    server_->close();
                for (auto& con : connections_)
                    con->force_disconnect();
                connections_.clear();
                subscriptions_.clear();
            });
            ioc_.stop();
        }
        if (thread_.joinable())
            thread_.join();
        server_.reset();
        ioc_.restart();
        running_.store(false);
    }

    nlohmann::json status_json(const std::string& instance) const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return {
            {"instance", instance},
            {"running", running_.load()},
            {"bind", bind_},
            {"port", port_},
            {"url", "mqtt://" + bind_ + ":" + std::to_string(port_)},
            {"accepts", accepts_.load()},
            {"connects", connects_.load()},
            {"publishes", publishes_.load()},
            {"subscriptions", subscriptions_count_.load()},
            {"error", error_},
        };
    }

private:
    void configure_server(MQTT_NS::server<>& server)
    {
        server.set_error_handler([this](MQTT_NS::error_code ec) {
            std::lock_guard<std::mutex> lk(mu_);
            error_ = ec.message();
        });

        server.set_accept_handler([this](MqttServerEndpointPtr spep) {
            ++accepts_;
            auto& ep = *spep;
            std::weak_ptr<MqttServerEndpoint> wp(spep);
            using packet_id_t = typename std::remove_reference_t<decltype(ep)>::packet_id_t;

            ep.start_session(std::move(spep));

            ep.set_close_handler([this, wp] {
                if (auto sp = wp.lock())
                    remove_connection(sp);
            });
            ep.set_error_handler([this, wp](MQTT_NS::error_code ec) {
                (void)ec;
                if (auto sp = wp.lock())
                    remove_connection(sp);
            });
            ep.set_connect_handler([this, wp](MQTT_NS::buffer,
                                              MQTT_NS::optional<MQTT_NS::buffer>,
                                              MQTT_NS::optional<MQTT_NS::buffer>,
                                              MQTT_NS::optional<MQTT_NS::will>,
                                              bool,
                                              std::uint16_t) {
                if (auto sp = wp.lock()) {
                    std::lock_guard<std::mutex> lk(mu_);
                    connections_.insert(sp);
                    ++connects_;
                    using connack_code_t = MQTT_NS::variant<MQTT_NS::connect_return_code, MQTT_NS::v5::connect_reason_code>;
                    sp->connack(false, connack_code_t{MQTT_NS::connect_return_code::accepted});
                }
                return true;
            });
            ep.set_disconnect_handler([this, wp] {
                if (auto sp = wp.lock())
                    remove_connection(sp);
            });
            ep.set_puback_handler([](packet_id_t) { return true; });
            ep.set_pubrec_handler([](packet_id_t) { return true; });
            ep.set_pubrel_handler([](packet_id_t) { return true; });
            ep.set_pubcomp_handler([](packet_id_t) { return true; });
            ep.set_publish_handler([this](MQTT_NS::optional<packet_id_t>,
                                          MQTT_NS::publish_options pubopts,
                                          MQTT_NS::buffer topic,
                                          MQTT_NS::buffer contents) {
                std::vector<MqttSubscription> targets;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    ++publishes_;
                    for (const auto& sub : subscriptions_)
                        if (sub.topic == topic)
                            targets.push_back(sub);
                }
                for (const auto& target : targets) {
                    target.endpoint->publish(
                        topic,
                        contents,
                        std::min(target.qos_value, pubopts.get_qos()));
                }
                return true;
            });
            ep.set_subscribe_handler([this, wp](packet_id_t packet_id,
                                                std::vector<MQTT_NS::subscribe_entry> entries) {
                std::vector<MQTT_NS::suback_return_code> results;
                auto sp = wp.lock();
                if (!sp)
                    return false;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    for (auto& entry : entries) {
                        subscriptions_.push_back({std::move(entry.topic_filter), sp, entry.subopts.get_qos()});
                        results.emplace_back(MQTT_NS::qos_to_suback_return_code(entry.subopts.get_qos()));
                        ++subscriptions_count_;
                    }
                }
                sp->suback(packet_id, results);
                return true;
            });
            ep.set_unsubscribe_handler([this, wp](packet_id_t packet_id,
                                                  std::vector<MQTT_NS::unsubscribe_entry> entries) {
                auto sp = wp.lock();
                if (!sp)
                    return false;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    for (const auto& entry : entries) {
                        subscriptions_.erase(
                            std::remove_if(subscriptions_.begin(), subscriptions_.end(),
                                           [&](const MqttSubscription& sub) {
                                               return sub.endpoint == sp && sub.topic == entry.topic_filter;
                                           }),
                            subscriptions_.end());
                    }
                }
                sp->unsuback(packet_id);
                return true;
            });
        });
    }

    void remove_connection(const MqttServerEndpointPtr& con)
    {
        std::lock_guard<std::mutex> lk(mu_);
        connections_.erase(con);
        subscriptions_.erase(
            std::remove_if(subscriptions_.begin(), subscriptions_.end(),
                           [&](const MqttSubscription& sub) { return sub.endpoint == con; }),
            subscriptions_.end());
    }

    mutable std::mutex mu_;
    boost::asio::io_context ioc_;
    std::unique_ptr<MQTT_NS::server<>> server_;
    std::thread thread_;
    std::set<MqttServerEndpointPtr> connections_;
    std::vector<MqttSubscription> subscriptions_;
    std::atomic_bool running_{false};
    std::atomic<long long> accepts_{0};
    std::atomic<long long> connects_{0};
    std::atomic<long long> publishes_{0};
    std::atomic<long long> subscriptions_count_{0};
    std::string bind_ = "127.0.0.1";
    int port_ = 1883;
    std::string error_;
};

std::mutex& mqtt_instances_mutex()
{
    static std::mutex mu;
    return mu;
}

std::map<std::string, std::shared_ptr<MqttServerInstance>>& mqtt_instances()
{
    static std::map<std::string, std::shared_ptr<MqttServerInstance>> instances;
    return instances;
}

nlohmann::json mqtt_messages_to_json(const std::vector<std::pair<std::string, std::string>>& messages)
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& [topic, payload] : messages)
        out.push_back({{"topic", topic}, {"payload", payload}});
    return out;
}

BlockResult mqtt_server_block(BlockIO& io)
{
    const std::string kind = "mqttServer";
    std::string action = media::strutil::lower_ascii(io.str("action", "Start"));
    if (action.empty())
        action = "start";
    std::string instance = io.str("instance", "default");
    if (instance.empty())
        instance = "default";

    if (action == "stop") {
        std::shared_ptr<MqttServerInstance> inst;
        {
            std::lock_guard<std::mutex> lk(mqtt_instances_mutex());
            auto it = mqtt_instances().find(instance);
            if (it != mqtt_instances().end()) {
                inst = it->second;
                mqtt_instances().erase(it);
            }
        }
        if (!inst) {
            io.warn(kind, "mqttServer: no broker instance named '" + instance + "'")
              .set("instance", instance);
            return BlockResult::Ok;
        }
        const nlohmann::json status = inst->status_json(instance);
        inst->stop();
        io.ok(kind, "stopped MQTT broker '" + instance + "'")
          .payload(status)
          .result(Value::from_json(status), ParamKind::json_value);
        return BlockResult::Ok;
    }

    if (action == "status") {
        std::shared_ptr<MqttServerInstance> inst;
        {
            std::lock_guard<std::mutex> lk(mqtt_instances_mutex());
            auto it = mqtt_instances().find(instance);
            if (it != mqtt_instances().end())
                inst = it->second;
        }
        nlohmann::json status = inst ? inst->status_json(instance)
                                     : nlohmann::json{{"instance", instance}, {"running", false}};
        io.ok(kind, "MQTT broker status for '" + instance + "'")
          .payload(status)
          .result(Value::from_json(std::move(status)), ParamKind::json_value);
        return BlockResult::Ok;
    }

    if (action != "start") {
        io.error(kind, "mqttServer: unknown action '" + action + "'", 1);
        return BlockResult::Error;
    }

    {
        std::lock_guard<std::mutex> lk(mqtt_instances_mutex());
        if (mqtt_instances().find(instance) != mqtt_instances().end()) {
            io.error(kind, "mqttServer: broker instance already running: " + instance, 1)
              .set("instance", instance);
            return BlockResult::Error;
        }
    }

    std::string bind = io.str("bind", "127.0.0.1");
    if (bind.empty())
        bind = "127.0.0.1";
    const int port = static_cast<int>(std::clamp<long long>(io.i64("port", 1883), 1, 65535));

    auto inst = std::make_shared<MqttServerInstance>();
    std::string err;
    if (!inst->start(bind, port, err)) {
        io.error(kind, "mqttServer: " + err, 1)
          .set("instance", instance)
          .set("bind", bind)
          .set("port", static_cast<long long>(port));
        return BlockResult::Error;
    }
    {
        std::lock_guard<std::mutex> lk(mqtt_instances_mutex());
        mqtt_instances()[instance] = inst;
    }

    nlohmann::json status = inst->status_json(instance);
    io.ok(kind, "started MQTT broker '" + instance + "': " + status.value("url", std::string{}))
      .set("instance", instance)
      .set("url", status.value("url", std::string{}))
      .payload(status)
      .result(Value::from_json(std::move(status)), ParamKind::json_value);
    return BlockResult::Ok;
}

BlockResult mqtt_client_block(BlockIO& io)
{
    const std::string kind = "mqttClient";
    std::string action = media::strutil::lower_ascii(io.str("action", "RoundTrip"));
    if (action.empty())
        action = "roundtrip";
    if (action != "publish" && action != "subscribe" && action != "roundtrip") {
        io.error(kind, "mqttClient: unknown action '" + action + "'", 1);
        return BlockResult::Error;
    }

    const std::string host = io.str("host", "127.0.0.1");
    const int port = static_cast<int>(std::clamp<long long>(io.i64("port", 1883), 1, 65535));
    const std::string topic = io.str("topic", "xblox/smoke");
    const std::string payload = io.str("payload", "hello from xblox");
    const int timeout_ms = static_cast<int>(std::max<long long>(1, io.i64("timeoutMs", io.runtime().options.default_timeout_ms)));
    media::net::MqttClientOptions opts;
    opts.host       = host;
    opts.port       = port;
    opts.client_id  = io.str("clientId", "xblox-mqtt");
    opts.action     = action;
    opts.topic      = topic;
    opts.payload    = payload;
    opts.qos        = static_cast<int>(io.i64("qos", 0));
    opts.timeout_ms = timeout_ms;
    opts.cancelled  = [&io] { return io.cancelled(); };
    const media::net::MqttClientResult result = media::net::mqtt_client_run(opts);

    nlohmann::json messages = mqtt_messages_to_json(result.messages);
    nlohmann::json meta{
        {"action", action},
        {"host", host},
        {"port", port},
        {"topic", topic},
        {"payload", payload},
        {"connected", result.connected},
        {"published", result.published},
        {"subscribed", result.subscribed},
        {"messages", messages},
    };
    if (!result.error.empty())
        meta["error"] = result.error;

    if (!result.ok) {
        io.error(kind, result.error.empty() ? "MQTT client failed" : result.error, 1)
          .payload(meta);
        return BlockResult::Error;
    }

    if (io.has_children("items")) {
        for (const auto& [message_topic, message_payload] : result.messages) {
            nlohmann::json message{
                {"topic", message_topic},
                {"payload", message_payload},
                {"host", host},
                {"port", port},
            };
            io.runtime().set_context_value(io.runtime().user_data, "mqttTopic", message_topic);
            io.runtime().set_context_value(io.runtime().user_data, "mqttPayload", message_payload);
            io.runtime().set_context_value(io.runtime().user_data, "mqttMessage", message);
            io.prime_previous(std::move(message));
            io.run("items");
            if (!io.runtime().result.ok)
                return BlockResult::Error;
        }
    }

    io.ok(kind, "MQTT " + action + " complete")
      .payload(meta)
      .result(Value::from_json(std::move(meta)), ParamKind::json_value);
    return BlockResult::Ok;
}

#endif

} // namespace

BlockHandler mqtt_block_handler(const std::string& kind)
{
#if defined(FEATURE_MQTT_XBLOX) && FEATURE_MQTT_XBLOX
    if (kind == "mqttServer") return mqtt_server_block;
    if (kind == "mqttClient") return mqtt_client_block;
#else
    (void)kind;
#endif
    return nullptr;
}

void register_mqtt_blocks(BlockRegistry& registry)
{
#if defined(FEATURE_MQTT_XBLOX) && FEATURE_MQTT_XBLOX
    registry["mqttServer"] = bdb(
        "mqttServer", mqtt_server_block, "MQTT Server", "MQTT",
        "Start, stop, or inspect a named in-process MQTT broker instance.",
        {{"kind","mqttServer"}, {"action","Start"}, {"instance","default"},
         {"bind","127.0.0.1"}, {"port",1883}, {"storeAs","mqttServer"}})
        .params({
            pd_enum("action", {"Start", "Stop", "Status"}).grp("server_lifecycle").dflt("Start")
                .desc("Start a named MQTT broker, Stop it, or report Status."),
            pd_str("instance").grp("server_lifecycle").dflt("default")
                .desc("Named broker instance. Use the same name for Stop/Status."),
            pd_str("bind").grp("network").dflt("127.0.0.1")
                .desc("Network interface to listen on."),
            pd_int("port").grp("network").dflt(1883).range(1, 65535)
                .desc("MQTT TCP port."),
            pd_store_as().dflt("mqttServer"),
        })
        .primary_output(ParamKind::json_value, "Server status", true)
        .exec({false, true, true});

    registry["mqttClient"] = bdb(
        "mqttClient", mqtt_client_block, "MQTT Client", "MQTT",
        "Connect to an MQTT broker and Publish, Subscribe, or RoundTrip a test message. "
        "For received messages, runs child items with PREVIOUS={topic,payload,host,port}.",
        {{"kind","mqttClient"}, {"action","RoundTrip"}, {"host","127.0.0.1"}, {"port",1883},
         {"topic","xblox/smoke"}, {"payload","hello from xblox"}, {"qos",0}, {"timeoutMs",5000},
         {"storeAs","mqtt"}, {"items", nlohmann::json::array()}})
        .params({
            pd_enum("action", {"Publish", "Subscribe", "RoundTrip"}).grp("mode").dflt("RoundTrip")
                .desc("Publish sends one message. Subscribe waits for one message. RoundTrip subscribes then publishes and waits for its echo."),
            pd_str("host").in().dflt("127.0.0.1").desc("Broker host."),
            pd_int("port").in().dflt(1883).range(1, 65535).desc("Broker port."),
            pd_str("clientId").adv().dflt("xblox-mqtt").desc("MQTT client id."),
            pd_str("topic").in().dflt("xblox/smoke").desc("MQTT topic."),
            pd_prompt("payload").in().dflt("hello from xblox").desc("Payload to publish."),
            pd_int("qos").adv().dflt(0).range(0, 2).desc("MQTT QoS level."),
            pd_ms("timeoutMs").grp("timeouts").dflt(5000).range(1, 60000).desc("Client operation timeout."),
            pd_store_as().dflt("mqtt"),
        })
        .primary_output(ParamKind::json_value, "Client result")
        .children({{"items", "On Message", "body", {"*"}, 0, -1}})
        .exec({false, true, true, true});
#else
    (void)registry;
#endif
}

} // namespace media::xblox::blocks
