// XBlox bluetooth blocks  -  bluetoothListDevices, bluetoothEndpoints,
//                          bluetoothConnect, bluetoothDisconnect,
//                          bluetoothPair, bluetoothUnpair, bluetoothSetDefault.
//
// Win32-only (classic BluetoothAPIs + MMDevice/IPolicyConfig). Gated on
// FEATURE_COMMAND_BLUETOOTH; on other builds the registry stays empty and no
// palette items are exposed. The pm::btaudio backend symbols are linked from
// pm-image / pm-image-cli (which compile src/win/bluetooth/*.cpp).
//
// PREVIOUS / storeAs contract:
//   • bluetoothListDevices -> array of {id,name,address,paired,connected,...}
//   • bluetoothEndpoints   -> array of {id,name,default,is_bluetooth,...}
//   • bluetoothConnect     -> { ok, target, routed_to }
//   • bluetoothDisconnect  -> { ok, target }
//   • bluetoothPair/Unpair -> { ok, target }
//   • bluetoothSetDefault  -> { ok, target }

#include "blocks/bluetooth_blocks.hpp"

#include <string>

#if defined(_WIN32) && defined(FEATURE_COMMAND_BLUETOOTH) && FEATURE_COMMAND_BLUETOOTH

#include "core/host_backends.hpp"

#include <chrono>
#include <thread>

#endif // _WIN32 && FEATURE_COMMAND_BLUETOOTH

namespace media::xblox::blocks {

#if defined(_WIN32) && defined(FEATURE_COMMAND_BLUETOOTH) && FEATURE_COMMAND_BLUETOOTH

namespace bt = pm::host::btaudio;

namespace {

// Implicit connect: route playback to a connected Bluetooth audio device's
// MMDevice endpoint. Returns the endpoint name we switched to (empty = none).
std::string implicit_route_to(const bt::BluetoothDeviceInfo& dev)
{
    if (!dev.is_audio_device || !dev.connected) return {};
    for (const auto& ep : bt::bluetooth_find_audio_endpoints(dev, /*capture=*/false)) {
        if (ep.is_default) return {};
        std::string err;
        if (bt::audio_set_default(ep.id, &err)) return ep.name;
    }
    return {};
}

nlohmann::json device_to_json(const bt::BluetoothDeviceInfo& d)
{
    return {
        {"id", d.id}, {"name", d.name}, {"address", d.address},
        {"paired", d.paired}, {"connected", d.connected},
        {"can_pair", d.can_pair}, {"is_audio_device", d.is_audio_device},
        {"device_class", d.device_class},
    };
}

// ── bluetoothListDevices ──────────────────────────────────────────────────────

BlockResult bluetooth_list_devices_block(BlockIO& io)
{
    const bool nearby     = io.flag("nearby", false);
    const bool auto_route = io.flag("autoRoute", false);

    std::string err;
    auto devices = bt::bluetooth_list_devices(nearby, &err);
    if (devices.empty() && !err.empty()) {
        io.error("bluetoothListDevices", "bluetoothListDevices: " + err, 1);
        return BlockResult::Error;
    }

    std::string routed;
    if (auto_route)
        for (const auto& d : devices)
            if (!(routed = implicit_route_to(d)).empty()) break;

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& d : devices) arr.push_back(device_to_json(d));

    const auto count = static_cast<long long>(arr.size());
    auto ev = io.ok("bluetoothListDevices",
                    "found " + std::to_string(count) + " bluetooth device(s)" +
                    (routed.empty() ? std::string{} : ", routed audio to \"" + routed + "\""));
    ev.set("count", count);
    if (!routed.empty()) ev.set("routedTo", routed);
    ev.result(Value::from_json(std::move(arr)), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── bluetoothEndpoints ────────────────────────────────────────────────────────

BlockResult bluetooth_endpoints_block(BlockIO& io)
{
    const bool capture             = io.flag("capture", false);
    const bool include_disconnected = io.flag("includeDisconnected", false);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : bt::audio_list_endpoints(capture, include_disconnected)) {
        // state: 1=active, 2=disabled, 4=not-present, 8=unplugged (DEVICE_STATE_*)
        arr.push_back({
            {"id", e.id}, {"name", e.name}, {"default", e.is_default},
            {"default_comm", e.is_default_comm}, {"is_bluetooth", e.is_bluetooth},
            {"capture", e.is_capture}, {"state", e.state},
            {"active", e.state == 1},
        });
    }
    const auto count = static_cast<long long>(arr.size());
    io.ok("bluetoothEndpoints", "found " + std::to_string(count) +
          (capture ? " recording endpoint(s)" : " playback endpoint(s)") +
          (include_disconnected ? " (including disconnected)" : ""))
      .set("count", count)
      .result(Value::from_json(std::move(arr)), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── bluetoothConnect ──────────────────────────────────────────────────────────

BlockResult bluetooth_connect_block(BlockIO& io)
{
    const std::string target = io.str("id");
    if (target.empty()) {
        io.error("bluetoothConnect", "bluetoothConnect: 'id' is empty (pass a device address or name, "
                 "or chain from bluetoothListDevices via PREVIOUS)", 2);
        return BlockResult::Error;
    }
    const bool auto_route = io.flag("autoRoute", true);
    const int  timeout_ms = std::max(0, static_cast<int>(io.i64("timeoutMs", 6000)));
    const int  delay_ms   = std::max(0, static_cast<int>(io.i64("delayMs",   0)));

    // Resolve the device record upfront.  We need the friendly name to match
    // against MMDevice endpoint names later (addresses don't appear there).
    bt::BluetoothDeviceInfo bdev;
    for (const auto& d : bt::bluetooth_list_devices(false, nullptr)) {
        if (d.id == target || d.name.find(target) != std::string::npos) {
            bdev = d;
            break;
        }
    }

    // Route backend diagnostics into the trace log for this call.
    bt::set_trace_sink([&io](const std::string& m) { io.trace("[bt] " + m); });

    // bluetooth_connect performs the full connect: find device, locate its
    // MMDevice audio endpoint(s) (even UNPLUGGED), drive A2DP via the KS
    // ONESHOT_RECONNECT IOCTL with event-driven wait (IMMNotificationClient),
    // and sets the render endpoint as default once it goes ACTIVE.
    // timeout_ms is passed through so the xblox block's timeoutMs config
    // controls how long to wait for slow devices (e.g. AV receivers booting).
    std::string err;
    const bool connect_ok = bt::bluetooth_connect(target, &err, timeout_ms);
    bt::set_trace_sink(nullptr);
    if (!connect_ok) {
        io.error("bluetoothConnect", "bluetoothConnect: " + err, 1).set("target", target);
        return BlockResult::Error;
    }
    if (!err.empty())
        io.warn("bluetoothConnect", "bluetoothConnect: " + err).set("target", target);

    // bluetooth_connect already waited for the endpoint to become ACTIVE and set
    // it as the default.  Do a single non-blocking lookup to populate routed_to.
    std::string routed;
    if (auto_route && !bdev.name.empty()) {
        for (const auto& ep : bt::bluetooth_find_audio_endpoints(bdev, /*capture=*/false)) {
            routed = ep.name;
            break;
        }
    }

    // Optional post-connect delay: AV receivers need time to switch inputs /
    // unmute after the BT link is established (separate from codec priming).
    if (delay_ms > 0) {
        io.trace("[bt] post-connect delay " + std::to_string(delay_ms) + " ms");
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(delay_ms);
        while (std::chrono::steady_clock::now() < deadline && !io.cancelled())
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    nlohmann::json result{{"ok", true}, {"target", target}};
    if (!routed.empty()) result["routed_to"] = routed;
    const std::string msg = routed.empty()
        ? "connect attempted for " + target + " (device may not be in range)"
        : "connected " + target + ", routed audio to \"" + routed + "\"";
    auto ev = io.ok("bluetoothConnect", msg);
    ev.set("target", target);
    if (!routed.empty()) ev.set("routedTo", routed);
    ev.result(Value::from_json(std::move(result)), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── shared single-target action (disconnect / pair / unpair / set-default) ─────

using BtAction = bool (*)(const std::string&, std::string*);

BlockResult run_action(BlockIO& io, const char* kind, BtAction action, const char* param)
{
    const std::string target = io.str(param);
    if (target.empty()) {
        io.error(kind, std::string(kind) + ": '" + param + "' is empty", 2);
        return BlockResult::Error;
    }
    std::string err;
    if (!action(target, &err)) {
        io.error(kind, std::string(kind) + ": " + err, 1).set("target", target);
        return BlockResult::Error;
    }
    io.ok(kind, std::string(kind) + ": OK (" + target + ")")
      .set("target", target)
      .result(Value::from_json(nlohmann::json{{"ok", true}, {"target", target}}), ParamKind::json_value);
    return BlockResult::Ok;
}

BlockResult bluetooth_disconnect_block(BlockIO& io) { return run_action(io, "bluetoothDisconnect", bt::bluetooth_disconnect, "id"); }
BlockResult bluetooth_pair_block(BlockIO& io)       { return run_action(io, "bluetoothPair",       bt::bluetooth_pair,       "id"); }
BlockResult bluetooth_unpair_block(BlockIO& io)     { return run_action(io, "bluetoothUnpair",     bt::bluetooth_unpair,     "id"); }

BlockResult bluetooth_set_default_block(BlockIO& io)
{
    return run_action(io, "bluetoothSetDefault", bt::audio_set_default, "id");
}

} // namespace

#endif // _WIN32 && FEATURE_COMMAND_BLUETOOTH

// ── registration ──────────────────────────────────────────────────────────────

void register_bluetooth_blocks(BlockRegistry& registry)
{
#if defined(_WIN32) && defined(FEATURE_COMMAND_BLUETOOTH) && FEATURE_COMMAND_BLUETOOTH
    registry["bluetoothListDevices"] = bdb(
        "bluetoothListDevices", bluetooth_list_devices_block,
        "Bluetooth Devices", "Bluetooth",
        "List paired / connected Bluetooth devices. Stores an array of "
        "{id,name,address,paired,connected,is_audio_device,...} in PREVIOUS / storeAs. "
        "Set autoRoute to implicitly switch the default playback endpoint to a connected audio device.",
        {{"kind","bluetoothListDevices"}, {"nearby",false}, {"autoRoute",false}, {"storeAs","bluetoothDevices"}})
        .params({
            pd_bool("nearby").adv().dflt(false)
                .desc("Also issue an inquiry for nearby discoverable devices (slow)."),
            pd_bool("autoRoute").adv().dflt(false)
                .desc("Implicitly route audio output to a connected Bluetooth audio device."),
            pd_store_as().dflt("bluetoothDevices"),
        })
        .children({{"items", "For each", "body", {"*"}, 0, -1}})
        .exec({false, false, false})
        .primary_output(ParamKind::json_value, "Devices")
        .iterable();

    registry["bluetoothEndpoints"] = bdb(
        "bluetoothEndpoints", bluetooth_endpoints_block,
        "Audio Endpoints", "Bluetooth",
        "List MMDevice audio endpoints (playback by default). Stores an array of "
        "{id,name,default,is_bluetooth,state,active,...} in PREVIOUS / storeAs. "
        "Set includeDisconnected to also list Bluetooth devices that are paired but not yet connected.",
        {{"kind","bluetoothEndpoints"}, {"capture",false}, {"includeDisconnected",false}, {"storeAs","audioEndpoints"}})
        .params({
            pd_bool("capture").adv().dflt(false)
                .desc("List recording (capture) endpoints instead of playback."),
            pd_bool("includeDisconnected").adv().dflt(false)
                .desc("Also include UNPLUGGED endpoints (paired Bluetooth devices not yet connected)."),
            pd_store_as().dflt("audioEndpoints"),
        })
        .children({{"items", "For each", "body", {"*"}, 0, -1}})
        .exec({false, false, false})
        .primary_output(ParamKind::json_value, "Endpoints")
        .iterable()
        .pure();  // read-only enumeration: safe to run under --simulate

    registry["bluetoothConnect"] = bdb(
        "bluetoothConnect", bluetooth_connect_block,
        "Bluetooth Connect", "Bluetooth",
        "Connect a Bluetooth audio device (pairs if needed) and implicitly route audio to it. "
        "Uses PREVIOUS as the device id/name when 'id' is empty. Stores {ok,target,routed_to} in PREVIOUS / storeAs.",
        {{"kind","bluetoothConnect"}, {"id",""}, {"autoRoute",true}, {"timeoutMs",6000}, {"delayMs",0}, {"storeAs","bluetooth"}})
        .params({
            pd_str("id").in().prev().dflt("")
                .desc("Device address (AA:BB:CC:DD:EE:FF) or name substring. Empty = use PREVIOUS."),
            pd_bool("autoRoute").adv().dflt(true)
                .desc("After connecting, set the device's audio endpoint as the default playback device."),
            pd_ms("timeoutMs").adv().dflt(6000).range(0, 60000)
                .desc("How long to wait (ms) for the audio endpoint to go ACTIVE."),
            pd_ms("delayMs").adv().dflt(0).range(0, 30000)
                .desc("Extra delay (ms) after connecting before the block returns. "
                      "Use for AV receivers that need time to switch inputs or unmute (0 = none)."),
            pd_store_as().dflt("bluetooth"),
        })
        .primary_output(ParamKind::json_value, "Connection result")
        .exec({false, true, true});

    registry["bluetoothDisconnect"] = bdb(
        "bluetoothDisconnect", bluetooth_disconnect_block,
        "Bluetooth Disconnect", "Bluetooth",
        "Disconnect a Bluetooth audio device (best-effort). Uses PREVIOUS when 'id' is empty.",
        {{"kind","bluetoothDisconnect"}, {"id",""}, {"storeAs","bluetooth"}})
        .params({
            pd_str("id").in().prev().dflt("").desc("Device address or name substring. Empty = use PREVIOUS."),
            pd_store_as().dflt("bluetooth"),
        })
        .primary_output(ParamKind::json_value, "Disconnect result")
        .exec({false, true, true});

    registry["bluetoothPair"] = bdb(
        "bluetoothPair", bluetooth_pair_block,
        "Bluetooth Pair", "Bluetooth",
        "Pair a Bluetooth device (just-works / SSP). Uses PREVIOUS when 'id' is empty.",
        {{"kind","bluetoothPair"}, {"id",""}, {"storeAs","bluetooth"}})
        .params({
            pd_str("id").in().prev().dflt("").desc("Device address or name substring. Empty = use PREVIOUS."),
            pd_store_as().dflt("bluetooth"),
        })
        .primary_output(ParamKind::json_value, "Pair result")
        .exec({false, true, true});

    registry["bluetoothUnpair"] = bdb(
        "bluetoothUnpair", bluetooth_unpair_block,
        "Bluetooth Unpair", "Bluetooth",
        "Remove (unpair) a Bluetooth device. Uses PREVIOUS when 'id' is empty.",
        {{"kind","bluetoothUnpair"}, {"id",""}, {"storeAs","bluetooth"}})
        .params({
            pd_str("id").in().prev().dflt("").desc("Device address or name substring. Empty = use PREVIOUS."),
            pd_store_as().dflt("bluetooth"),
        })
        .primary_output(ParamKind::json_value, "Unpair result")
        .exec({false, true, true});

    registry["bluetoothSetDefault"] = bdb(
        "bluetoothSetDefault", bluetooth_set_default_block,
        "Set Default Endpoint", "Bluetooth",
        "Set an MMDevice audio endpoint as the default device for all roles. "
        "Pass the endpoint id from bluetoothEndpoints (or PREVIOUS).",
        {{"kind","bluetoothSetDefault"}, {"id",""}, {"storeAs","bluetooth"}})
        .params({
            pd_str("id").in().prev().dflt("")
                .desc("MMDevice endpoint id (e.g. \"{0.0.0.00000000}.{guid}\"). Empty = use PREVIOUS."),
            pd_store_as().dflt("bluetooth"),
        })
        .primary_output(ParamKind::json_value, "Endpoint result")
        .exec({false, true, true});
#else
    (void)registry;
#endif
}

} // namespace media::xblox::blocks
