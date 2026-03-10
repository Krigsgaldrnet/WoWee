#include "network/world_socket.hpp"
#include "network/packet.hpp"
#include "network/net_platform.hpp"
#include "auth/crypto.hpp"
#include "core/logger.hpp"
#include <iomanip>
#include <sstream>
#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <cstring>

namespace {
constexpr size_t kMaxReceiveBufferBytes = 8 * 1024 * 1024;
constexpr int kMaxParsedPacketsPerUpdate = 220;

inline bool isLoginPipelineSmsg(uint16_t opcode) {
    switch (opcode) {
        case 0x1EC: // SMSG_AUTH_CHALLENGE
        case 0x1EE: // SMSG_AUTH_RESPONSE
        case 0x03B: // SMSG_CHAR_ENUM
        case 0x03A: // SMSG_CHAR_CREATE
        case 0x03C: // SMSG_CHAR_DELETE
        case 0x4AB: // SMSG_CLIENTCACHE_VERSION
        case 0x0FD: // SMSG_TUTORIAL_FLAGS
        case 0x2E6: // SMSG_WARDEN_DATA
            return true;
        default:
            return false;
    }
}

inline bool isLoginPipelineCmsg(uint16_t opcode) {
    switch (opcode) {
        case 0x1ED: // CMSG_AUTH_SESSION
        case 0x037: // CMSG_CHAR_ENUM
        case 0x036: // CMSG_CHAR_CREATE
        case 0x038: // CMSG_CHAR_DELETE
        case 0x03D: // CMSG_PLAYER_LOGIN
            return true;
        default:
            return false;
    }
}

inline bool envFlagEnabled(const char* key, bool defaultValue = false) {
    const char* raw = std::getenv(key);
    if (!raw || !*raw) return defaultValue;
    return !(raw[0] == '0' || raw[0] == 'f' || raw[0] == 'F' ||
             raw[0] == 'n' || raw[0] == 'N');
}
} // namespace

namespace wowee {
namespace network {

// WoW 3.3.5a RC4 encryption keys (hardcoded in client)
static const uint8_t ENCRYPT_KEY[] = {
    0xC2, 0xB3, 0x72, 0x3C, 0xC6, 0xAE, 0xD9, 0xB5,
    0x34, 0x3C, 0x53, 0xEE, 0x2F, 0x43, 0x67, 0xCE
};

static const uint8_t DECRYPT_KEY[] = {
    0xCC, 0x98, 0xAE, 0x04, 0xE8, 0x97, 0xEA, 0xCA,
    0x12, 0xDD, 0xC0, 0x93, 0x42, 0x91, 0x53, 0x57
};

WorldSocket::WorldSocket() {
    net::ensureInit();
    // Always reserve baseline receive capacity (safe, behavior-preserving).
    receiveBuffer.reserve(64 * 1024);
    useFastRecvAppend_ = envFlagEnabled("WOWEE_NET_FAST_RECV_APPEND", true);
    useParseScratchQueue_ = envFlagEnabled("WOWEE_NET_PARSE_SCRATCH", false);
    if (useParseScratchQueue_) {
        LOG_WARNING("WOWEE_NET_PARSE_SCRATCH is temporarily disabled (known unstable); forcing off");
        useParseScratchQueue_ = false;
    }
    if (useParseScratchQueue_) {
        parsedPacketsScratch_.reserve(64);
    }
    LOG_INFO("WorldSocket net opts: fast_recv_append=", useFastRecvAppend_ ? "on" : "off",
             " parse_scratch=", useParseScratchQueue_ ? "on" : "off");
}

WorldSocket::~WorldSocket() {
    WorldSocket::disconnect();  // qualified call: virtual dispatch is bypassed in destructors
}

bool WorldSocket::connect(const std::string& host, uint16_t port) {
    LOG_INFO("Connecting to world server: ", host, ":", port);

    // Create socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == INVALID_SOCK) {
        LOG_ERROR("Failed to create socket");
        return false;
    }

    // Set non-blocking
    net::setNonBlocking(sockfd);

    // Resolve host
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
        LOG_ERROR("Failed to resolve host: ", host);
        net::closeSocket(sockfd);
        sockfd = INVALID_SOCK;
        return false;
    }

    // Connect
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr = reinterpret_cast<struct sockaddr_in*>(res->ai_addr)->sin_addr;
    serverAddr.sin_port = htons(port);
    freeaddrinfo(res);

    int result = ::connect(sockfd, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
    if (result < 0) {
        int err = net::lastError();
        if (!net::isInProgress(err)) {
            LOG_ERROR("Failed to connect: ", net::errorString(err));
            net::closeSocket(sockfd);
            sockfd = INVALID_SOCK;
            return false;
        }

        // Non-blocking connect in progress — wait up to 10s for completion.
        // On Windows, calling recv() before the connect completes returns
        // WSAENOTCONN; we must poll writability before declaring connected.
        fd_set writefds, errfds;
        FD_ZERO(&writefds);
        FD_ZERO(&errfds);
        FD_SET(sockfd, &writefds);
        FD_SET(sockfd, &errfds);

        struct timeval tv;
        tv.tv_sec  = 10;
        tv.tv_usec = 0;

        int sel = ::select(static_cast<int>(sockfd) + 1, nullptr, &writefds, &errfds, &tv);
        if (sel <= 0) {
            LOG_ERROR("World server connection timed out (", host, ":", port, ")");
            net::closeSocket(sockfd);
            sockfd = INVALID_SOCK;
            return false;
        }

        // Verify the socket error code — writeable doesn't guarantee success on all platforms
        int sockErr = 0;
        socklen_t errLen = sizeof(sockErr);
        getsockopt(sockfd, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&sockErr), &errLen);
        if (sockErr != 0) {
            LOG_ERROR("Failed to connect to world server: ", net::errorString(sockErr));
            net::closeSocket(sockfd);
            sockfd = INVALID_SOCK;
            return false;
        }
    }

    connected = true;
    LOG_INFO("Connected to world server: ", host, ":", port);
    return true;
}

void WorldSocket::disconnect() {
    if (sockfd != INVALID_SOCK) {
        net::closeSocket(sockfd);
        sockfd = INVALID_SOCK;
    }
    connected = false;
    encryptionEnabled = false;
    useVanillaCrypt = false;
    receiveBuffer.clear();
    receiveReadOffset_ = 0;
    parsedPacketsScratch_.clear();
    headerBytesDecrypted = 0;
    LOG_INFO("Disconnected from world server");
}

bool WorldSocket::isConnected() const {
    return connected;
}

void WorldSocket::send(const Packet& packet) {
    if (!connected) return;
    static const bool kLogCharCreatePayload = envFlagEnabled("WOWEE_NET_LOG_CHAR_CREATE", false);
    static const bool kLogSwapItemPackets = envFlagEnabled("WOWEE_NET_LOG_SWAP_ITEM", false);

    const auto& data = packet.getData();
    uint16_t opcode = packet.getOpcode();
    uint16_t payloadLen = static_cast<uint16_t>(data.size());

    // Debug: parse and log character-create payload fields (helps diagnose appearance issues).
    if (kLogCharCreatePayload && opcode == 0x036) { // CMSG_CHAR_CREATE
        size_t pos = 0;
        std::string name;
        while (pos < data.size()) {
            uint8_t c = data[pos++];
            if (c == 0) break;
            name.push_back(static_cast<char>(c));
        }
        auto rd8 = [&](uint8_t& out) -> bool {
            if (pos >= data.size()) return false;
            out = data[pos++];
            return true;
        };
        uint8_t race = 0, cls = 0, gender = 0;
        uint8_t skin = 0, face = 0, hairStyle = 0, hairColor = 0, facial = 0, outfit = 0;
        bool ok =
            rd8(race) && rd8(cls) && rd8(gender) &&
            rd8(skin) && rd8(face) && rd8(hairStyle) && rd8(hairColor) && rd8(facial) && rd8(outfit);
        if (ok) {
            LOG_INFO("CMSG_CHAR_CREATE payload: name='", name,
                     "' race=", (int)race, " class=", (int)cls, " gender=", (int)gender,
                     " skin=", (int)skin, " face=", (int)face,
                     " hairStyle=", (int)hairStyle, " hairColor=", (int)hairColor,
                     " facial=", (int)facial, " outfit=", (int)outfit,
                     " payloadLen=", payloadLen);
            // Persist to disk so we can compare TX vs DB even if the console scrolls away.
            std::ofstream f("charcreate_payload.log", std::ios::app);
            if (f.is_open()) {
                f << "name='" << name << "'"
                  << " race=" << (int)race
                  << " class=" << (int)cls
                  << " gender=" << (int)gender
                  << " skin=" << (int)skin
                  << " face=" << (int)face
                  << " hairStyle=" << (int)hairStyle
                  << " hairColor=" << (int)hairColor
                  << " facial=" << (int)facial
                  << " outfit=" << (int)outfit
                  << " payloadLen=" << payloadLen
                  << "\n";
            }
        } else {
            LOG_WARNING("CMSG_CHAR_CREATE payload too short to parse (name='", name,
                        "' payloadLen=", payloadLen, " pos=", pos, ")");
        }
    }

    if (kLogSwapItemPackets && (opcode == 0x10C || opcode == 0x10D)) { // CMSG_SWAP_ITEM / CMSG_SWAP_INV_ITEM
        std::string hex;
        for (size_t i = 0; i < data.size(); i++) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02x ", data[i]);
            hex += buf;
        }
        LOG_INFO("WS TX opcode=0x", std::hex, opcode, std::dec, " payloadLen=", payloadLen, " data=[", hex, "]");
    }

    // WotLK 3.3.5 CMSG header (6 bytes total):
    // - size (2 bytes, big-endian) = payloadLen + 4 (opcode is 4 bytes for CMSG)
    // - opcode (4 bytes, little-endian)
    // Note: Client-to-server uses 4-byte opcode, server-to-client uses 2-byte
    uint16_t sizeField = payloadLen + 4;

    std::vector<uint8_t> sendData;
    sendData.reserve(6 + payloadLen);

    // Size (2 bytes, big-endian)
    uint8_t size_hi = (sizeField >> 8) & 0xFF;
    uint8_t size_lo = sizeField & 0xFF;
    sendData.push_back(size_hi);
    sendData.push_back(size_lo);

    // Opcode (4 bytes, little-endian)
    sendData.push_back(opcode & 0xFF);
    sendData.push_back((opcode >> 8) & 0xFF);
    sendData.push_back(0);  // High bytes are 0 for all WoW opcodes
    sendData.push_back(0);

    // Debug logging disabled - too spammy

    // Encrypt header if encryption is enabled (all 6 bytes)
    if (encryptionEnabled) {
        if (useVanillaCrypt) {
            vanillaCrypt.encrypt(sendData.data(), 6);
        } else {
            encryptCipher.process(sendData.data(), 6);
        }
    }

    // Add payload (unencrypted)
    sendData.insert(sendData.end(), data.begin(), data.end());

    // Debug: dump packet bytes for AUTH_SESSION
    if (opcode == 0x1ED) {
        std::string hexDump = "AUTH_SESSION raw bytes: ";
        for (size_t i = 0; i < sendData.size(); ++i) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02x ", sendData[i]);
            hexDump += buf;
            if ((i + 1) % 32 == 0) hexDump += "\n";
        }
        LOG_DEBUG(hexDump);
    }
    if (isLoginPipelineCmsg(opcode)) {
        LOG_INFO("WS TX LOGIN opcode=0x", std::hex, opcode, std::dec,
                 " payload=", payloadLen, " enc=", encryptionEnabled ? "yes" : "no");
    }

    // Send complete packet
    ssize_t sent = net::portableSend(sockfd, sendData.data(), sendData.size());
    if (sent < 0) {
        LOG_ERROR("Send failed: ", net::errorString(net::lastError()));
    } else {
        if (static_cast<size_t>(sent) != sendData.size()) {
            LOG_WARNING("Partial send: ", sent, " of ", sendData.size(), " bytes");
        }
    }
}

void WorldSocket::update() {
    if (!connected) return;
    auto bufferedBytes = [&]() -> size_t {
        return (receiveBuffer.size() >= receiveReadOffset_)
            ? (receiveBuffer.size() - receiveReadOffset_)
            : 0;
    };
    auto compactReceiveBuffer = [&]() {
        if (receiveReadOffset_ == 0) return;
        if (receiveReadOffset_ >= receiveBuffer.size()) {
            receiveBuffer.clear();
            receiveReadOffset_ = 0;
            return;
        }
        const size_t remaining = receiveBuffer.size() - receiveReadOffset_;
        std::memmove(receiveBuffer.data(), receiveBuffer.data() + receiveReadOffset_, remaining);
        receiveBuffer.resize(remaining);
        receiveReadOffset_ = 0;
    };

    // Drain the socket. Some servers send an auth response and immediately close; a single recv()
    // may read the response, and a subsequent recv() can return 0 (FIN). If we disconnect right
    // away we lose the buffered response and the UI ends up with a generic "no characters" symptom.
    bool sawClose = false;
    bool receivedAny = false;
    size_t bytesReadThisTick = 0;
    int readOps = 0;
    while (connected) {
        uint8_t buffer[4096];
        ssize_t received = net::portableRecv(sockfd, buffer, sizeof(buffer));

        if (received > 0) {
            receivedAny = true;
            ++readOps;
            size_t receivedSize = static_cast<size_t>(received);
            bytesReadThisTick += receivedSize;
            if (useFastRecvAppend_) {
                size_t liveBytes = bufferedBytes();
                if (liveBytes > kMaxReceiveBufferBytes || receivedSize > (kMaxReceiveBufferBytes - liveBytes)) {
                    compactReceiveBuffer();
                    liveBytes = bufferedBytes();
                }
                if (liveBytes > kMaxReceiveBufferBytes || receivedSize > (kMaxReceiveBufferBytes - liveBytes)) {
                    LOG_ERROR("World socket receive buffer would overflow (buffered=", liveBytes,
                              " incoming=", receivedSize, " max=", kMaxReceiveBufferBytes,
                              "). Disconnecting to recover framing.");
                    disconnect();
                    return;
                }
                const size_t oldSize = receiveBuffer.size();
                const size_t needed = oldSize + receivedSize;
                if (receiveBuffer.capacity() < needed) {
                    size_t newCap = receiveBuffer.capacity() ? receiveBuffer.capacity() : 64 * 1024;
                    while (newCap < needed && newCap < kMaxReceiveBufferBytes) {
                        newCap = std::min(kMaxReceiveBufferBytes, newCap * 2);
                    }
                    if (newCap < needed) {
                        LOG_ERROR("World socket receive buffer capacity growth failed (needed=", needed,
                                  " max=", kMaxReceiveBufferBytes, "). Disconnecting to recover framing.");
                        disconnect();
                        return;
                    }
                    receiveBuffer.reserve(newCap);
                }
                receiveBuffer.insert(receiveBuffer.end(), buffer, buffer + receivedSize);
            } else {
                receiveBuffer.insert(receiveBuffer.end(), buffer, buffer + received);
            }
            if (bufferedBytes() > kMaxReceiveBufferBytes) {
                LOG_ERROR("World socket receive buffer overflow (", bufferedBytes(),
                          " bytes). Disconnecting to recover framing.");
                disconnect();
                return;
            }
            continue;
        }

        if (received == 0) {
            sawClose = true;
            break;
        }

        int err = net::lastError();
        if (net::isWouldBlock(err)) {
            break;
        }
        if (net::isConnectionClosed(err)) {
            // Peer closed the connection — treat the same as recv() returning 0
            sawClose = true;
            break;
        }

        LOG_ERROR("Receive failed: ", net::errorString(err));
        disconnect();
        return;
    }

    if (receivedAny) {
        const bool debugLog = core::Logger::getInstance().shouldLog(core::LogLevel::DEBUG);
        if (debugLog) {
            LOG_DEBUG("World socket read ", bytesReadThisTick, " bytes in ", readOps,
                      " recv call(s), buffered=", bufferedBytes());
        }
        // Hex dump received bytes for auth debugging (debug-only to avoid per-frame string work)
        if (debugLog && bytesReadThisTick <= 128) {
            std::string hex;
            for (size_t i = receiveReadOffset_; i < receiveBuffer.size(); ++i) {
                char buf[4]; snprintf(buf, sizeof(buf), "%02x ", receiveBuffer[i]); hex += buf;
            }
            LOG_DEBUG("World socket raw bytes: ", hex);
        }
        tryParsePackets();
        if (debugLog && connected && bufferedBytes() > 0) {
            LOG_DEBUG("World socket parse left ", bufferedBytes(),
                     " bytes buffered (awaiting complete packet)");
        }
    }

    if (sawClose) {
        LOG_INFO("World server connection closed (receivedAny=", receivedAny,
                 " buffered=", bufferedBytes(), ")");
        disconnect();
        return;
    }
}

void WorldSocket::tryParsePackets() {
    // World server packets have 4-byte incoming header: size(2) + opcode(2)
    int parsedThisTick = 0;
    size_t parseOffset = receiveReadOffset_;
    size_t localHeaderBytesDecrypted = headerBytesDecrypted;
    std::vector<Packet> parsedPacketsLocal;
    std::vector<Packet>* parsedPackets = &parsedPacketsLocal;
    if (useParseScratchQueue_) {
        parsedPacketsScratch_.clear();
        // Keep a warm queue to reduce steady-state allocations, but avoid
        // retaining pathological capacity after burst/misaligned streams.
        if (parsedPacketsScratch_.capacity() > 1024) {
            std::vector<Packet>().swap(parsedPacketsScratch_);
        } else if (parsedPacketsScratch_.capacity() < 64) {
            parsedPacketsScratch_.reserve(64);
        }
        parsedPackets = &parsedPacketsScratch_;
    } else {
        parsedPacketsLocal.reserve(32);
    }
    while ((receiveBuffer.size() - parseOffset) >= 4 && parsedThisTick < kMaxParsedPacketsPerUpdate) {
        uint8_t rawHeader[4] = {0, 0, 0, 0};
        std::memcpy(rawHeader, receiveBuffer.data() + parseOffset, 4);

        // Decrypt header bytes in-place if encryption is enabled
        // Only decrypt bytes we haven't already decrypted
        if (encryptionEnabled && localHeaderBytesDecrypted < 4) {
            size_t toDecrypt = 4 - localHeaderBytesDecrypted;
            if (useVanillaCrypt) {
                vanillaCrypt.decrypt(receiveBuffer.data() + parseOffset + localHeaderBytesDecrypted, toDecrypt);
            } else {
                decryptCipher.process(receiveBuffer.data() + parseOffset + localHeaderBytesDecrypted, toDecrypt);
            }
            localHeaderBytesDecrypted = 4;
        }

        // Parse header (now decrypted in-place).
        // Size: 2 bytes big-endian. For world packets, this includes opcode bytes.
        uint16_t size = (receiveBuffer[parseOffset + 0] << 8) | receiveBuffer[parseOffset + 1];
        // Opcode: 2 bytes little-endian.
        uint16_t opcode = receiveBuffer[parseOffset + 2] | (receiveBuffer[parseOffset + 3] << 8);
        if (size < 2) {
            LOG_ERROR("World packet framing desync: invalid size=", size,
                      " rawHdr=", std::hex,
                      static_cast<int>(rawHeader[0]), " ",
                      static_cast<int>(rawHeader[1]), " ",
                      static_cast<int>(rawHeader[2]), " ",
                      static_cast<int>(rawHeader[3]), std::dec,
                      " enc=", encryptionEnabled, ". Disconnecting to recover stream.");
            disconnect();
            return;
        }
        constexpr uint16_t kMaxWorldPacketSize = 0x4000;
        if (size > kMaxWorldPacketSize) {
            LOG_ERROR("World packet framing desync: oversized packet size=", size,
                      " rawHdr=", std::hex,
                      static_cast<int>(rawHeader[0]), " ",
                      static_cast<int>(rawHeader[1]), " ",
                      static_cast<int>(rawHeader[2]), " ",
                      static_cast<int>(rawHeader[3]), std::dec,
                      " enc=", encryptionEnabled, ". Disconnecting to recover stream.");
            disconnect();
            return;
        }

        const uint16_t payloadLen = size - 2;
        const size_t totalSize = 4 + payloadLen;

        if (headerTracePacketsLeft > 0) {
            LOG_INFO("WS HDR TRACE raw=",
                     std::hex,
                     static_cast<int>(rawHeader[0]), " ",
                     static_cast<int>(rawHeader[1]), " ",
                     static_cast<int>(rawHeader[2]), " ",
                     static_cast<int>(rawHeader[3]),
                     " dec=",
                     static_cast<int>(receiveBuffer[parseOffset + 0]), " ",
                     static_cast<int>(receiveBuffer[parseOffset + 1]), " ",
                     static_cast<int>(receiveBuffer[parseOffset + 2]), " ",
                     static_cast<int>(receiveBuffer[parseOffset + 3]),
                     std::dec,
                     " size=", size,
                     " payload=", payloadLen,
                     " opcode=0x", std::hex, opcode, std::dec,
                     " buffered=", (receiveBuffer.size() - parseOffset));
            --headerTracePacketsLeft;
        }
        if (isLoginPipelineSmsg(opcode)) {
            LOG_INFO("WS RX LOGIN opcode=0x", std::hex, opcode, std::dec,
                     " size=", size, " payload=", payloadLen,
                     " buffered=", (receiveBuffer.size() - parseOffset),
                     " enc=", encryptionEnabled ? "yes" : "no");
        }

        if ((receiveBuffer.size() - parseOffset) < totalSize) {
            // Not enough data yet - header stays decrypted in buffer
            break;
        }

        // Extract payload (skip header). Guard allocation failures so malformed
        // streams cannot unwind into application-level OOM crashes.
        try {
            std::vector<uint8_t> packetData(payloadLen);
            if (payloadLen > 0) {
                std::memcpy(packetData.data(), receiveBuffer.data() + parseOffset + 4, payloadLen);
            }
            // Queue packet; callbacks run after buffer state is finalized.
            parsedPackets->emplace_back(opcode, std::move(packetData));
        } catch (const std::bad_alloc& e) {
            LOG_ERROR("OOM while queuing world packet opcode=0x", std::hex, opcode, std::dec,
                      " payload=", payloadLen, " buffered=", receiveBuffer.size(),
                      " parseOffset=", parseOffset, " what=", e.what(),
                      ". Disconnecting to recover.");
            disconnect();
            return;
        }
        parseOffset += totalSize;
        localHeaderBytesDecrypted = 0;
        ++parsedThisTick;
    }

    if (parseOffset > receiveReadOffset_) {
        receiveReadOffset_ = parseOffset;
        // Compact lazily to avoid front-erase memmove every update.
        if (receiveReadOffset_ >= receiveBuffer.size()) {
            receiveBuffer.clear();
            receiveReadOffset_ = 0;
        } else if (receiveReadOffset_ >= 64 * 1024 || receiveReadOffset_ * 2 >= receiveBuffer.size()) {
            const size_t remaining = receiveBuffer.size() - receiveReadOffset_;
            std::memmove(receiveBuffer.data(), receiveBuffer.data() + receiveReadOffset_, remaining);
            receiveBuffer.resize(remaining);
            receiveReadOffset_ = 0;
        }
    }
    headerBytesDecrypted = localHeaderBytesDecrypted;

    if (packetCallback) {
        for (const auto& packet : *parsedPackets) {
            if (!connected) break;
            packetCallback(packet);
        }
    }

    const size_t buffered = (receiveBuffer.size() >= receiveReadOffset_)
        ? (receiveBuffer.size() - receiveReadOffset_)
        : 0;
    if (parsedThisTick >= kMaxParsedPacketsPerUpdate && buffered >= 4) {
        LOG_DEBUG("World socket parse budget reached (", parsedThisTick,
                 " packets); deferring remaining buffered data=", buffered, " bytes");
    }
}

void WorldSocket::initEncryption(const std::vector<uint8_t>& sessionKey, uint32_t build) {
    if (sessionKey.size() != 40) {
        LOG_ERROR("Invalid session key size: ", sessionKey.size(), " (expected 40)");
        return;
    }

    // Vanilla/TBC (build <= 8606) uses XOR+addition cipher with raw session key
    // WotLK (build > 8606) uses HMAC-SHA1 derived RC4 with 1024-byte drop
    useVanillaCrypt = (build <= 8606);

    LOG_INFO(">>> ENABLING ENCRYPTION (", useVanillaCrypt ? "vanilla XOR" : "WotLK RC4",
             ") build=", build, " <<<");

    if (useVanillaCrypt) {
        vanillaCrypt.init(sessionKey);
    } else {
        // WotLK: HMAC-SHA1(hardcoded seed, sessionKey) → RC4 key
        std::vector<uint8_t> encryptKey(ENCRYPT_KEY, ENCRYPT_KEY + 16);
        std::vector<uint8_t> decryptKey(DECRYPT_KEY, DECRYPT_KEY + 16);

        std::vector<uint8_t> encryptHash = auth::Crypto::hmacSHA1(encryptKey, sessionKey);
        std::vector<uint8_t> decryptHash = auth::Crypto::hmacSHA1(decryptKey, sessionKey);

        // WoW WotLK world-header stream cipher is protocol-defined RC4.
        // Replacing it would break interoperability with target servers.
        encryptCipher.init(encryptHash); // codeql[cpp/weak-cryptographic-algorithm]
        decryptCipher.init(decryptHash); // codeql[cpp/weak-cryptographic-algorithm]

        // Drop first 1024 bytes of keystream (WoW WotLK protocol requirement)
        encryptCipher.drop(1024);
        decryptCipher.drop(1024);
    }

    encryptionEnabled = true;
    headerTracePacketsLeft = 24;
    LOG_INFO("World server encryption initialized successfully");
}

} // namespace network
} // namespace wowee
