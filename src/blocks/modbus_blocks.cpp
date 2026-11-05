// XBlox Modbus blocks  -  modbusServer / modbusReadHoldingRegisters /
// modbusWriteRegister / modbusWriteRegisters.
//
// Migrated to the native handler surface (no legacy adapter): pure
// BlockResult(BlockIO&) handlers reading pre-resolved typed inputs and
// publishing typed, kind-tagged outputs.
//   • inputs    -  io.str/i64/flag/json over the resolver's frame (${var} already
//                interpolated for the string `url`).
//   • primary   -  io.ok(...).result(value) -> PREVIOUS/storeAs.
//   • metadata  -  io.ok(...).payload({...}) for the structured register/stat dump.
//   • failure   -  io.error(...) + return BlockResult::Error so the executor's
//                error policy (continueOnError/abortOnError) decides flow.
//   • cancel    -  run_tcp_server polls io.cancelled().

#include "blocks/modbus_blocks.hpp"

#include "modbus/modbus_args.hpp"
#include "modbus/modbus_client.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "string/string_utils.hpp"

namespace media::xblox::blocks {
namespace {

using media::strutil::lower_ascii;

using media::modbus::bit_values;
using media::modbus::uint16_values;

nlohmann::json uint16_json(const std::vector<std::uint16_t>& values)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto value : values)
        arr.push_back(value);
    return arr;
}

nlohmann::json bit_json(const std::vector<std::uint8_t>& values)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto value : values)
        arr.push_back(value ? 1 : 0);
    return arr;
}

std::string read_area_from_io(BlockIO& io, const std::string& fallback = "holdingRegisters")
{
    std::string area = io.has("area") ? io.str("area") : std::string{};
    if (area.empty() && io.has("type")) area = io.str("type");
    if (area.empty() && io.has("table")) area = io.str("table");
    if (area.empty() && io.has("function")) area = io.str("function");
    if (area.empty() && io.has("functionCode")) area = io.str("functionCode");
    return media::modbus::normalize_area(area, fallback);
}

std::string write_area_from_io(BlockIO& io, const std::string& fallback = "holdingRegisters")
{
    return media::modbus::normalize_write_area(read_area_from_io(io, fallback), fallback);
}

int count_from_io(BlockIO& io, int dflt = 1)
{
    if (io.has("count"))
        return std::max(1, static_cast<int>(io.i64("count", dflt)));
    if (io.has("registerCount"))
        return std::max(1, static_cast<int>(io.i64("registerCount", dflt)));
    if (io.has("regCount"))
        return std::max(1, static_cast<int>(io.i64("regCount", dflt)));
    if (io.has("quantity"))
        return std::max(1, static_cast<int>(io.i64("quantity", dflt)));
    return std::max(1, dflt);
}

int normalize_address(int address, const std::string& area, BlockIO& io)
{
    return media::modbus::normalize_address(address, area, io.str("addressBase", "zero"));
}

std::vector<std::uint16_t> register_values_from_io(BlockIO& io)
{
    auto values = uint16_values(io.json("values"));
    if (values.empty())
        values = uint16_values(io.json("value"));
    return values;
}

std::vector<std::uint8_t> coil_values_from_io(BlockIO& io)
{
    auto values = bit_values(io.json("values"));
    if (values.empty())
        values = bit_values(io.json("value"));
    return values;
}

std::string connection_name_from_io(BlockIO& io)
{
    std::string name = io.str_any({"connection", "instance", "client"});
    if (name == "none" || name == "false")
        name.clear();
    return name;
}

std::string connection_cache_key(const std::string& name)
{
    return "modbusConnection:" + name;
}

struct PooledModbusConnection {
    media::modbus::ClientConnection client;
    std::string url;
    int slave = 0;
    int timeout_ms = 30000;
    std::string error;

    bool open(const std::string& next_url, int next_slave, int next_timeout_ms, bool force, std::string& err)
    {
        if (!force && client.connected() && url == next_url && slave == next_slave && timeout_ms == next_timeout_ms)
            return true;
        if (!client.open(next_url, next_slave, next_timeout_ms, err)) {
            error = err;
            return false;
        }
        url = next_url;
        slave = next_slave;
        timeout_ms = next_timeout_ms;
        error.clear();
        return true;
    }

    nlohmann::json status(const std::string& name) const
    {
        return {
            {"name", name},
            {"connected", client.connected()},
            {"url", url},
            {"slave", slave},
            {"timeoutMs", timeout_ms},
            {"operations", client.operation_count()},
            {"error", error},
        };
    }
};

PooledModbusConnection* acquire_modbus_connection(BlockIO& io,
                                                  const std::string& name,
                                                  const std::string& url,
                                                  int slave,
                                                  int timeout_ms,
                                                  bool force,
                                                  std::string& err)
{
    RunScope* scope = io.scope();
    if (!scope) {
        err = "named Modbus connections require a run scope";
        return nullptr;
    }
    auto& pooled = scope->acquire<PooledModbusConnection>(connection_cache_key(name), [] {
        return std::make_shared<PooledModbusConnection>();
    });
    if (!pooled.open(url, slave, timeout_ms, force, err)) {
        scope->release(connection_cache_key(name));
        return nullptr;
    }
    return &pooled;
}

std::string modbus_url_from_io(BlockIO& io, const std::string& connection_name)
{
    if (io.has("url"))
        return io.str("url");
    if (!connection_name.empty()) {
        if (RunScope* scope = io.scope()) {
            if (auto* pooled = scope->find<PooledModbusConnection>(connection_cache_key(connection_name)))
                return pooled->url;
        }
    }
    return io.str("url");
}

media::modbus::ServerOptions server_options_from_io(BlockIO& io, std::string& err)
{
    media::modbus::ServerOptions options;
    const std::string url = io.str("url", "tcp:127.0.0.1:15020");
    if (!media::modbus::parse_endpoint(url, options.endpoint, err))
        return options;
    options.unit_id = static_cast<int>(io.i64("unitId", io.i64("slave", options.endpoint.slave)));
    options.holding_register_count = static_cast<int>(io.i64("registerCount", 128));
    options.input_register_count = static_cast<int>(io.i64("inputRegisterCount", options.holding_register_count));
    options.coil_count = static_cast<int>(io.i64("coilCount", 128));
    options.discrete_input_count = static_cast<int>(io.i64("discreteInputCount", options.coil_count));
    options.holding_registers = uint16_values(io.json("holdingRegisters"));
    if (options.holding_registers.empty())
        options.holding_registers = uint16_values(io.json("registers"));
    options.input_registers = uint16_values(io.json("inputRegisters"));
    options.coils = bit_values(io.json("coils"));
    options.discrete_inputs = bit_values(io.json("discreteInputs"));
    if (options.discrete_inputs.empty())
        options.discrete_inputs = bit_values(io.json("inputs"));
    options.duration_ms = static_cast<int>(io.i64("durationMs", io.i64("duration_ms", 0)));
    options.max_requests = static_cast<int>(io.i64("maxRequests", io.i64("max_requests", 0)));
    options.poll_timeout_ms = static_cast<int>(io.i64("pollTimeoutMs", 100));
    options.debug = io.flag("debug");
    return options;
}

BlockResult modbus_server_block(BlockIO& io)
{
    const std::string id = io.str("id");
    std::string err;
    auto options = server_options_from_io(io, err);
    if (!err.empty()) {
        io.error("modbusServer", "modbusServer: " + err, 1).id(id).type("modbus");
        return BlockResult::Error;
    }

    media::modbus::ServerStats stats;
    const bool ok = media::modbus::run_tcp_server(options,
        [&io]() { return io.cancelled(); },
        stats);

    nlohmann::json meta{
        {"tool", "modbus_server"},
        {"url", stats.url},
        {"requests", stats.requests},
        {"replies", stats.replies},
        {"connections", stats.connections},
        {"elapsedMs", stats.elapsed_ms},
    };
    if (!stats.error.empty())
        meta["error"] = stats.error;

    if (!ok) {
        io.error("modbusServer", stats.error.empty() ? "modbus server failed" : stats.error, 1)
          .id(id).type("modbus").payload(std::move(meta));
        return BlockResult::Error;
    }

    io.ok("modbusServer", "modbus server stopped")
      .id(id).type("modbus").payload(std::move(meta))
      .result(Value::from_json(nlohmann::json{
          {"url", stats.url},
          {"requests", stats.requests},
          {"replies", stats.replies},
          {"connections", stats.connections},
      }), ParamKind::json_value);
    return BlockResult::Ok;
}

BlockResult modbus_connection_block(BlockIO& io)
{
    const std::string id = io.str("id");
    std::string action = lower_ascii(io.str("action", "Start"));
    if (action.empty())
        action = "start";
    std::string name = connection_name_from_io(io);
    if (name.empty())
        name = "default";

    if (action == "stop") {
        if (RunScope* scope = io.scope())
            scope->release(connection_cache_key(name));
        io.ok("modbusConnection", "stopped Modbus connection '" + name + "'")
          .id(id).type("modbus")
          .result(Value::from_json(nlohmann::json{{"name", name}, {"connected", false}}), ParamKind::json_value);
        return BlockResult::Ok;
    }

    if (action == "status") {
        nlohmann::json status{{"name", name}, {"connected", false}};
        if (RunScope* scope = io.scope()) {
            if (auto* pooled = scope->find<PooledModbusConnection>(connection_cache_key(name)))
                status = pooled->status(name);
        }
        io.ok("modbusConnection", "Modbus connection status for '" + name + "'")
          .id(id).type("modbus")
          .payload(status)
          .result(Value::from_json(std::move(status)), ParamKind::json_value);
        return BlockResult::Ok;
    }

    if (action != "start") {
        io.error("modbusConnection", "modbusConnection: unknown action '" + action + "'", 2).id(id).type("modbus");
        return BlockResult::Error;
    }

    const std::string url = io.str("url", "tcp:127.0.0.1:15020");
    const int slave = static_cast<int>(io.i64("slave", io.i64("unitId", 0)));
    const int timeout_ms = static_cast<int>(io.i64("timeoutMs", io.runtime().options.default_timeout_ms));
    std::string err;
    PooledModbusConnection* pooled = acquire_modbus_connection(io, name, url, slave, timeout_ms, io.flag("reload", false), err);
    if (!pooled) {
        io.error("modbusConnection", "modbusConnection: " + err, 1).id(id).type("modbus");
        return BlockResult::Error;
    }
    nlohmann::json status = pooled->status(name);
    io.ok("modbusConnection", "started Modbus connection '" + name + "'")
      .id(id).type("modbus")
      .payload(status)
      .result(Value::from_json(std::move(status)), ParamKind::json_value);
    return BlockResult::Ok;
}

BlockResult modbus_read_block(BlockIO& io)
{
    const std::string kind = io.str("kind", "modbusRead");
    const std::string id = io.str("id");
    const std::string connection_name = connection_name_from_io(io);
    const std::string url = modbus_url_from_io(io, connection_name);
    if (url.empty()) {
        io.error(kind, kind + ": 'url' is required", 2).id(id).type("modbus");
        return BlockResult::Error;
    }
    std::string area = read_area_from_io(io, kind == "modbusReadCoils" ? "coils" :
                                             kind == "modbusReadInputRegisters" ? "inputRegisters" :
                                             kind == "modbusReadDiscreteInputs" ? "discreteInputs" :
                                             "holdingRegisters");
    const int address    = normalize_address(static_cast<int>(io.i64("address", 0)), area, io);
    const int count      = count_from_io(io);
    const int slave      = static_cast<int>(io.i64("slave", io.i64("unitId", 0)));
    const int timeout_ms = static_cast<int>(io.i64("timeoutMs", io.runtime().options.default_timeout_ms));
    const int wait_ms    = std::max(0, static_cast<int>(io.i64("waitTimeMs", 20)));

    std::string err;
    PooledModbusConnection* pooled = nullptr;
    if (!connection_name.empty()) {
        pooled = acquire_modbus_connection(io, connection_name, url, slave, timeout_ms, io.flag("reload", false), err);
        if (!pooled) {
            io.error(kind, kind + ": " + err, 1).id(id).type("modbus");
            return BlockResult::Error;
        }
    }

    nlohmann::json arr = nlohmann::json::array();
    if (area == "coils" || area == "discreteInputs") {
        std::vector<std::uint8_t> values;
        const bool ok = pooled
            ? (area == "coils"
                ? pooled->client.read_coils(address, count, values, err)
                : pooled->client.read_discrete_inputs(address, count, values, err))
            : (area == "coils"
                ? media::modbus::read_coils(url, address, count, slave, timeout_ms, values, err)
                : media::modbus::read_discrete_inputs(url, address, count, slave, timeout_ms, values, err));
        if (!ok) {
            io.error(kind, kind + ": " + err, 1).id(id).type("modbus");
            return BlockResult::Error;
        }
        arr = bit_json(values);
    } else {
        std::vector<std::uint16_t> values;
        const bool ok = pooled
            ? (area == "inputRegisters"
                ? pooled->client.read_input_registers(address, count, values, err)
                : pooled->client.read_holding_registers(address, count, values, err))
            : (area == "inputRegisters"
                ? media::modbus::read_input_registers(url, address, count, slave, timeout_ms, values, err)
                : media::modbus::read_holding_registers(url, address, count, slave, timeout_ms, values, err));
        if (!ok) {
            io.error(kind, kind + ": " + err, 1).id(id).type("modbus");
            return BlockResult::Error;
        }
        arr = uint16_json(values);
    }
    if (wait_ms > 0 && io.wait(wait_ms))
        return BlockResult::Fatal;
    io.ok(kind, "modbus " + area + " read")
      .id(id).type("modbus")
      .payload({{"tool", "modbus_read"}, {"area", area}, {"url", url}, {"connection", connection_name},
                {"pooled", pooled != nullptr}, {"address", address}, {"count", count}, {"waitTimeMs", wait_ms}, {"values", arr}})
      .result(Value::from_json(arr), ParamKind::json_value);
    return BlockResult::Ok;
}

BlockResult modbus_write_block(BlockIO& io)
{
    const std::string kind = io.str("kind", "modbusWrite");
    const std::string id = io.str("id");
    const std::string connection_name = connection_name_from_io(io);
    const std::string url = modbus_url_from_io(io, connection_name);
    if (url.empty()) {
        io.error(kind, kind + ": 'url' is required", 2).id(id).type("modbus");
        return BlockResult::Error;
    }
    std::string area = write_area_from_io(io, (kind == "modbusWriteCoil" || kind == "modbusWriteCoils") ? "coils" : "holdingRegisters");
    const int address      = normalize_address(static_cast<int>(io.i64("address", 0)), area, io);
    const int slave        = static_cast<int>(io.i64("slave", io.i64("unitId", 0)));
    const int timeout_ms   = static_cast<int>(io.i64("timeoutMs", io.runtime().options.default_timeout_ms));
    const int wait_ms      = std::max(0, static_cast<int>(io.i64("waitTimeMs", 20)));

    std::string err;
    PooledModbusConnection* pooled = nullptr;
    if (!connection_name.empty()) {
        pooled = acquire_modbus_connection(io, connection_name, url, slave, timeout_ms, io.flag("reload", false), err);
        if (!pooled) {
            io.error(kind, kind + ": " + err, 1).id(id).type("modbus");
            return BlockResult::Error;
        }
    }

    nlohmann::json values_json = nlohmann::json::array();
    if (area == "coils") {
        const auto values = coil_values_from_io(io);
        if (values.empty()) {
            io.error(kind, kind + ": 'value' or 'values' is required", 2).id(id).type("modbus");
            return BlockResult::Error;
        }
        const bool ok = pooled
            ? (values.size() == 1
                ? pooled->client.write_coil(address, values[0] != 0, err)
                : pooled->client.write_coils(address, values, err))
            : (values.size() == 1
                ? media::modbus::write_coil(url, address, values[0] != 0, slave, timeout_ms, err)
                : media::modbus::write_coils(url, address, values, slave, timeout_ms, err));
        if (!ok) {
            io.error(kind, kind + ": " + err, 1).id(id).type("modbus");
            return BlockResult::Error;
        }
        values_json = bit_json(values);
    } else {
        const auto values = register_values_from_io(io);
        if (values.empty()) {
            io.error(kind, kind + ": 'value' or 'values' is required", 2).id(id).type("modbus");
            return BlockResult::Error;
        }
        const bool ok = pooled
            ? (values.size() == 1
                ? pooled->client.write_register(address, values[0], err)
                : pooled->client.write_registers(address, values, err))
            : (values.size() == 1
                ? media::modbus::write_register(url, address, values[0], slave, timeout_ms, err)
                : media::modbus::write_registers(url, address, values, slave, timeout_ms, err));
        if (!ok) {
            io.error(kind, kind + ": " + err, 1).id(id).type("modbus");
            return BlockResult::Error;
        }
        values_json = uint16_json(values);
    }
    if (wait_ms > 0 && io.wait(wait_ms))
        return BlockResult::Fatal;

    io.ok(kind, "modbus " + area + " written")
      .id(id).type("modbus")
      .payload({{"tool", "modbus_write"}, {"area", area}, {"url", url}, {"connection", connection_name},
                {"pooled", pooled != nullptr}, {"address", address}, {"waitTimeMs", wait_ms}, {"values", values_json}})
      .result(Value::from_json(nlohmann::json{{"area", area}, {"address", address}, {"values", values_json}}), ParamKind::json_value);
    return BlockResult::Ok;
}

} // namespace

BlockHandler modbus_block_handler(const std::string& kind)
{
    if (kind == "modbusServer")                 return modbus_server_block;
    if (kind == "modbusConnection")             return modbus_connection_block;
    if (kind == "modbusRead")                   return modbus_read_block;
    if (kind == "modbusReadHoldingRegisters")   return modbus_read_block;
    if (kind == "modbusReadInputRegisters")     return modbus_read_block;
    if (kind == "modbusReadCoils")              return modbus_read_block;
    if (kind == "modbusReadDiscreteInputs")     return modbus_read_block;
    if (kind == "modbusWrite")                  return modbus_write_block;
    if (kind == "modbusWriteRegister")          return modbus_write_block;
    if (kind == "modbusWriteRegisters")         return modbus_write_block;
    if (kind == "modbusWriteCoil")              return modbus_write_block;
    if (kind == "modbusWriteCoils")             return modbus_write_block;
    return nullptr;
}

void register_modbus_blocks(BlockRegistry& registry)
{
    registry["modbusServer"] = bdb(
        "modbusServer", modbus_server_block, "Modbus Server", "Modbus",
        "Run a Modbus TCP server until cancelled, durationMs expires, or maxRequests is reached.",
        {{"kind","modbusServer"},{"url","tcp:127.0.0.1:15020"},{"holdingRegisters",{11,22,33}},
         {"inputRegisters",{44,55,66}},{"coils",{1,0,1}},{"discreteInputs",{0,1,0}},{"durationMs",3000}})
        .params({
            pd_str("url").in().dflt("tcp:127.0.0.1:15020").desc("Listen endpoint, e.g. tcp:127.0.0.1:15020."),
            pd_json("holdingRegisters").in().desc("Initial holding-register values (integer array)."),
            pd_json("inputRegisters").in().desc("Initial input-register values (integer array)."),
            pd_int("registerCount").adv().dflt(128).range(0, 65535).desc("Number of holding registers to expose."),
            pd_int("inputRegisterCount").adv().dflt(128).range(0, 65535).desc("Number of input registers to expose."),
            pd_int("coilCount").adv().dflt(128).range(0, 65535).desc("Number of coils to expose."),
            pd_json("coils").adv().desc("Initial coil values (boolean/integer array)."),
            pd_int("discreteInputCount").adv().dflt(128).range(0, 65535).desc("Number of discrete inputs to expose."),
            pd_json("discreteInputs").adv().desc("Initial discrete-input values (boolean/integer array)."),
            pd_ms("durationMs").adv().dflt(0).range(0, 3600000).desc("Auto-stop after this many ms. 0 = run until cancelled."),
            pd_int("maxRequests").adv().dflt(0).range(0, 1000000).desc("Auto-stop after this many requests. 0 = unlimited."),
            pd_int("unitId").adv().dflt(1).range(0, 255).desc("Modbus unit/slave id."),
            pd_int("pollTimeoutMs").adv().dflt(100).range(1, 60000).desc("Server poll timeout in ms."),
            pd_bool("debug").adv().dflt(false).desc("Verbose server logging."),
            pd_store_as(),
        })
        .primary_output(ParamKind::json_value, "Server result")
        .exec({false, true, true});

    registry["modbusConnection"] = bdb(
        "modbusConnection", modbus_connection_block, "Modbus Connection", "Modbus",
        "Start, stop, or inspect a named cached Modbus client connection for fast repeated reads/writes.",
        {{"kind","modbusConnection"},{"action","Start"},{"connection","default"},{"url","tcp:127.0.0.1:15020"},{"storeAs","modbusConnection"}})
        .params({
            pd_enum("action", {"Start", "Stop", "Status"}).grp("connection").dflt("Start")
                .desc("Start/open a named connection, Stop/release it, or report Status."),
            pd_str("connection").grp("connection").dflt("default")
                .desc("Named connection/pool key reused by read/write blocks."),
            pd_str("instance").adv().grp("connection").dflt("")
                .desc("Alias for connection."),
            pd_str("url").in().dflt("tcp:127.0.0.1:15020").desc("Endpoint to connect to."),
            pd_int("slave").adv().range(0, 255).desc("Unit/slave id."),
            pd_int("unitId").adv().range(0, 255).desc("Alias for slave."),
            pd_ms("timeoutMs").adv().dflt(30000).range(0, 3600000),
            pd_bool("reload").adv().dflt(false).desc("Force reconnect even if the named connection is already open."),
            pd_store_as().dflt("modbusConnection"),
        })
        .primary_output(ParamKind::json_value, "Connection status")
        .exec({false, true, true});

    const std::vector<ParamDef> read_params = {
        pd_str("url").in().req().dflt("tcp:127.0.0.1:15020").desc("Endpoint to read from."),
        pd_str("connection").grp("connection").dflt("").desc("Optional named connection to reuse/open for fast repeated reads."),
        pd_str("instance").adv().grp("connection").dflt("").desc("Alias for connection."),
        pd_bool("reload").adv().grp("connection").dflt(false).desc("Force reconnect the named connection before this operation."),
        pd_enum("area", {"holdingRegisters", "inputRegisters", "coils", "discreteInputs"}).in().dflt("holdingRegisters")
            .desc("Modbus table/function: holdingRegisters=FC03, inputRegisters=FC04, coils=FC01, discreteInputs=FC02."),
        pd_int("address").in().dflt(0).range(0, 65535)
            .desc("Starting address. Zero-based by default; 40001/30001/10001 style addresses are also accepted."),
        pd_int("count").in().dflt(1).range(1, 2000).desc("Number of coils/registers to read."),
        pd_int("regCount").in().dflt(1).range(1, 2000).desc("Alias for count, matching ModbusPoll wording."),
        pd_str("addressBase").adv().dflt("zero").desc("zero (default), one, or modbus. one subtracts 1 from address."),
        pd_int("slave").adv().range(0, 255).desc("Unit/slave id."),
        pd_ms("timeoutMs").adv().dflt(30000).range(0, 3600000),
        pd_ms("waitTimeMs").adv().dflt(20).range(0, 60000)
            .desc("Cancellable delay after a successful read before the next block. 0 = no delay."),
        pd_store_as().dflt("values"),
    };

    registry["modbusRead"] = bdb(
        "modbusRead", modbus_read_block, "Modbus Read", "Modbus",
        "Read coils, discrete inputs, holding registers, or input registers. ModbusPoll-style aliases: area/functionCode, address, count/regCount.",
        {{"kind","modbusRead"},{"url","tcp:127.0.0.1:15020"},{"area","holdingRegisters"},{"address",0},{"count",3},{"storeAs","values"}})
        .params(read_params)
        .primary_output(ParamKind::json_value, "Values")
        .exec({false, false, true});

    registry["modbusReadHoldingRegisters"] = bdb(
        "modbusReadHoldingRegisters", modbus_read_block, "Modbus Read Holding Registers", "Modbus",
        "Read holding registers from a Modbus TCP/RTU endpoint.",
        {{"kind","modbusReadHoldingRegisters"},{"url","tcp:127.0.0.1:15020"},{"address",0},{"count",3},{"storeAs","values"}})
        .params(read_params)
        .primary_output(ParamKind::json_value, "Values")
        .exec({false, false, true});

    registry["modbusReadInputRegisters"] = bdb(
        "modbusReadInputRegisters", modbus_read_block, "Modbus Read Input Registers", "Modbus",
        "Read input registers (FC04).",
        {{"kind","modbusReadInputRegisters"},{"url","tcp:127.0.0.1:15020"},{"area","inputRegisters"},{"address",0},{"count",3},{"storeAs","values"}})
        .params(read_params)
        .primary_output(ParamKind::json_value, "Values")
        .exec({false, false, true});

    registry["modbusReadCoils"] = bdb(
        "modbusReadCoils", modbus_read_block, "Modbus Read Coils", "Modbus",
        "Read coils (FC01).",
        {{"kind","modbusReadCoils"},{"url","tcp:127.0.0.1:15020"},{"area","coils"},{"address",0},{"count",8},{"storeAs","values"}})
        .params(read_params)
        .primary_output(ParamKind::json_value, "Values")
        .exec({false, false, true});

    registry["modbusReadDiscreteInputs"] = bdb(
        "modbusReadDiscreteInputs", modbus_read_block, "Modbus Read Discrete Inputs", "Modbus",
        "Read discrete inputs (FC02).",
        {{"kind","modbusReadDiscreteInputs"},{"url","tcp:127.0.0.1:15020"},{"area","discreteInputs"},{"address",0},{"count",8},{"storeAs","values"}})
        .params(read_params)
        .primary_output(ParamKind::json_value, "Values")
        .exec({false, false, true});

    const std::vector<ParamDef> write_params = {
        pd_str("url").in().req().dflt("tcp:127.0.0.1:15020").desc("Endpoint to write to."),
        pd_str("connection").grp("connection").dflt("").desc("Optional named connection to reuse/open for fast repeated writes."),
        pd_str("instance").adv().grp("connection").dflt("").desc("Alias for connection."),
        pd_bool("reload").adv().grp("connection").dflt(false).desc("Force reconnect the named connection before this operation."),
        pd_enum("area", {"holdingRegisters", "coils"}).in().dflt("holdingRegisters")
            .desc("Writable table/function: holdingRegisters=FC06/FC16, coils=FC05/FC15."),
        pd_int("address").in().dflt(0).range(0, 65535)
            .desc("Starting address. Zero-based by default; 40001/00001 style addresses are also accepted."),
        pd_json("value").in().desc("Single 16-bit register value or coil boolean/0/1."),
        pd_json("values").in().desc("Multiple values. Arrays and comma-separated strings are accepted."),
        pd_str("addressBase").adv().dflt("zero").desc("zero (default), one, or modbus. one subtracts 1 from address."),
        pd_int("slave").adv().range(0, 255).desc("Unit/slave id."),
        pd_ms("timeoutMs").adv().dflt(30000).range(0, 3600000),
        pd_ms("waitTimeMs").adv().dflt(20).range(0, 60000)
            .desc("Cancellable delay after a successful write before the next block. 0 = no delay."),
        pd_store_as(),
    };

    registry["modbusWrite"] = bdb(
        "modbusWrite", modbus_write_block, "Modbus Write", "Modbus",
        "Write one or many holding registers/coils. Chooses FC06/FC16 or FC05/FC15 based on area and value count.",
        {{"kind","modbusWrite"},{"url","tcp:127.0.0.1:15020"},{"area","holdingRegisters"},{"address",0},{"values",{101,202}}})
        .params(write_params)
        .primary_output(ParamKind::json_value, "Write result")
        .exec({false, false, true});

    registry["modbusWriteRegister"] = bdb(
        "modbusWriteRegister", modbus_write_block, "Modbus Write Register", "Modbus",
        "Write a single holding register.",
        {{"kind","modbusWriteRegister"},{"url","tcp:127.0.0.1:15020"},{"address",0},{"value",123}})
        .params(write_params)
        .primary_output(ParamKind::json_value, "Write result")
        .exec({false, false, true});

    registry["modbusWriteRegisters"] = bdb(
        "modbusWriteRegisters", modbus_write_block, "Modbus Write Registers", "Modbus",
        "Write multiple holding registers.",
        {{"kind","modbusWriteRegisters"},{"url","tcp:127.0.0.1:15020"},{"address",0},{"values",{101,202}}})
        .params(write_params)
        .primary_output(ParamKind::json_value, "Write result")
        .exec({false, false, true});

    registry["modbusWriteCoil"] = bdb(
        "modbusWriteCoil", modbus_write_block, "Modbus Write Coil", "Modbus",
        "Write a single coil (FC05).",
        {{"kind","modbusWriteCoil"},{"url","tcp:127.0.0.1:15020"},{"area","coils"},{"address",0},{"value",1}})
        .params(write_params)
        .primary_output(ParamKind::json_value, "Write result")
        .exec({false, false, true});

    registry["modbusWriteCoils"] = bdb(
        "modbusWriteCoils", modbus_write_block, "Modbus Write Coils", "Modbus",
        "Write multiple coils (FC15).",
        {{"kind","modbusWriteCoils"},{"url","tcp:127.0.0.1:15020"},{"area","coils"},{"address",0},{"values",{1,0,1}}})
        .params(write_params)
        .primary_output(ParamKind::json_value, "Write result")
        .exec({false, false, true});
}

} // namespace media::xblox::blocks
