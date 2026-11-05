#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace media::xblox::constants {

// Session identity hashing.
inline constexpr std::uint64_t kFnv1a64OffsetBasis = 1469598103934665603ull;
inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ull;

// Session transport labels.
inline constexpr const char* kTransportTcp = "tcp";
inline constexpr const char* kTransportUds = "uds";

// Length-prefixed IPC frames. This protects the control socket from unbounded
// allocations while still leaving room for batched XBlox messages.
inline constexpr std::uint32_t kIpcMaxFrameBytes = 1u << 20; // 1 MiB

// Typed IPC message protocol used by hot send/receive paths.
inline constexpr const char* kIpcTypedMagic = "XBIPC1";
inline constexpr std::size_t kIpcTypedMagicLen = 6;
inline constexpr char kIpcTypedSendBatch = 'B';
inline constexpr char kIpcTypedRecv = 'R';
inline constexpr char kIpcTypedSendAck = 'b';
inline constexpr char kIpcTypedMessages = 'M';

// Runtime IPC caches and buffering.
inline constexpr std::chrono::seconds kIpcTargetCacheTtl{30};
inline constexpr std::size_t kIpcInboxCap = 8192;

// Typed IPC frame sizing hints. These are reserve estimates only.
inline constexpr std::size_t kIpcTypedSendBatchReserveFixedBytes = 5;
inline constexpr std::size_t kIpcTypedRecvReserveFixedBytes = 2;
inline constexpr std::size_t kIpcTypedSendAckReserveFixedBytes = 17;
inline constexpr std::size_t kIpcTypedMessagesReserveFixedBytes = 5;
inline constexpr std::size_t kIpcTypedSendBatchReservePerMessageBytes = 32;
inline constexpr std::size_t kIpcTypedMessagesReservePerMessageBytes = 40;

// XBlox IPC block defaults.
inline constexpr int kIpcDefaultTimeoutMs = 3000;
inline constexpr long long kIpcTimeoutMaxMs = 300000;
inline constexpr long long kIpcMaxGeneratedBatchMessages = 10000;
inline constexpr long long kIpcDefaultWaitMs = 0;
inline constexpr long long kIpcWaitMaxMs = 300000;
inline constexpr long long kIpcDefaultPollMs = 100;
inline constexpr long long kIpcPollMinMs = 10;
inline constexpr long long kIpcPollMaxMs = 60000;

// Core runtime defaults.
inline constexpr int kDefaultMaxLoopIterations = 10000;
inline constexpr int kDefaultTimeoutMs = 30000;
inline constexpr long long kLoopIntervalMaxMs = 24LL * 60 * 60 * 1000; // 1 day

// Flow block defaults/ranges.
inline constexpr int kWhileFastPathCancelPollInterval = 4096;
inline constexpr long long kWaitDefaultMs = 500;
inline constexpr long long kWaitMaxMs = 3600000;
inline constexpr long long kLoopLimitDefault = 10;
inline constexpr long long kLoopLimitMax = 1000000;

// HTTP/network block defaults and ranges.
inline constexpr long long kNetworkDefaultTimeoutMs = kDefaultTimeoutMs;
inline constexpr long long kNetworkDefaultConnectTimeoutMs = 10000;
inline constexpr long long kNetworkTimeoutMaxMs = 300000;
inline constexpr long long kNetworkDefaultMaxRedirects = 5;
inline constexpr long long kNetworkMaxRedirects = 30;
inline constexpr long long kNetworkDefaultRetries = 2;
inline constexpr long long kNetworkMaxRetries = 10;
inline constexpr long long kNetworkDefaultCacheTtlMs = 3600000;
inline constexpr long long kNetworkCacheTtlMinMs = -1;
inline constexpr long long kNetworkCacheTtlMaxMs = 86400000;
inline constexpr long long kNetworkDefaultRetryDelayMs = 250;
inline constexpr long long kNetworkRetryDelayMaxMs = 60000;

// Shell/command execution defaults.
inline constexpr long long kShellDefaultTimeoutMs = kDefaultTimeoutMs;
inline constexpr long long kShellTimeoutMaxMs = 3600000;
inline constexpr std::size_t kCommandPipeReadBufferBytes = 4096;

} // namespace media::xblox::constants
