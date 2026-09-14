#include "essentials_tcp_bridge.h"

#include "json.h"
#include "supabase.h"

#include <rtc/rtc.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace aml::essentials {

namespace {

constexpr uintptr_t kInvalidSocket = static_cast<uintptr_t>(~0ull);

SOCKET as_socket(uintptr_t value) {
    return static_cast<SOCKET>(value);
}

bool send_all(SOCKET socket, const char* data, size_t size) {
    while (size > 0) {
        const int sent = send(socket, data, static_cast<int>(std::min<size_t>(size, 64 * 1024)), 0);
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

bool send_all(SOCKET socket, const rtc::binary& data) {
    return send_all(socket, reinterpret_cast<const char*>(data.data()), data.size());
}

}  // namespace

TcpDataChannelBridge::TcpDataChannelBridge() {
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
}

TcpDataChannelBridge::~TcpDataChannelBridge() {
    stop();
    WSACleanup();
}

bool TcpDataChannelBridge::start_host(const std::string& session_id, uint16_t minecraft_port) {
    return start(Role::Host, session_id, {}, minecraft_port);
}

bool TcpDataChannelBridge::start_guest(const std::string& session_id,
                                       const std::string& host_user_id,
                                       uint16_t requested_port) {
    return start(Role::Guest, session_id, host_user_id, requested_port);
}

bool TcpDataChannelBridge::start(Role role, const std::string& session_id,
                                 const std::string& host_user_id, uint16_t port) {
    stop();
    auto& supabase = supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated() || !supabase.client()) {
        set_error("Amalgam account authentication is required for multiplayer");
        return false;
    }

    role_ = role;
    session_id_ = session_id;
    host_user_id_ = host_user_id;
    minecraft_port_ = role == Role::Host ? port : 0;
    local_port_ = 0;
    local_user_id_ = supabase.get_current_user().id;
    remote_user_id_.clear();
    error_.clear();

    if (role == Role::Guest) {
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) {
            set_error("Could not create the local Minecraft TCP adapter");
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
            listen(listener, 1) == SOCKET_ERROR) {
            closesocket(listener);
            set_error("Could not bind the local Minecraft TCP adapter");
            return false;
        }
        int address_size = sizeof(address);
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_size);
        local_port_ = ntohs(address.sin_port);
        DWORD timeout = 250;
        setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        listener_socket_ = static_cast<uintptr_t>(listener);
    }

    running_ = true;
    signal_thread_ = std::thread(&TcpDataChannelBridge::poll_signals, this);
    io_thread_ = std::thread(&TcpDataChannelBridge::io_loop, this);

    if (role == Role::Guest) {
        Json hello = Json::obj();
        hello.set("session_id", Json::str(session_id_));
        publish(host_user_id, "hello", hello.dump());
    }
    return true;
}

void TcpDataChannelBridge::stop() {
    const bool was_running = running_.exchange(false);
    if (!was_running && !signal_thread_.joinable() && !io_thread_.joinable()) return;
    if (signal_thread_.joinable()) signal_thread_.join();
    if (io_thread_.joinable()) io_thread_.join();

    std::shared_ptr<rtc::PeerConnection> peer;
    {
        std::lock_guard<std::mutex> lock(mu_);
        peer = std::move(peer_);
        channel_.reset();
    }
    if (peer) peer->close();
    close_socket();
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (listener_socket_ != kInvalidSocket) {
            closesocket(as_socket(listener_socket_));
            listener_socket_ = kInvalidSocket;
        }
    }
    ready_ = false;
}

bool TcpDataChannelBridge::is_ready() const {
    return ready_.load();
}

uint16_t TcpDataChannelBridge::local_port() const {
    std::lock_guard<std::mutex> lock(mu_);
    return local_port_;
}

std::string TcpDataChannelBridge::error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return error_;
}

void TcpDataChannelBridge::set_error(const std::string& message) {
    std::lock_guard<std::mutex> lock(mu_);
    error_ = message;
}

void TcpDataChannelBridge::publish(const std::string& recipient_id,
                                   const std::string& kind,
                                   const std::string& payload) {
    supabase::EssentialsSignal signal;
    signal.session_id = session_id_;
    signal.sender_id = local_user_id_;
    signal.recipient_id = recipient_id;
    signal.kind = kind;
    signal.payload = payload;
    if (!supabase::SupabaseManager::instance().publish_essentials_signal(signal)) {
        set_error("Could not publish multiplayer signaling data");
    }
}

void TcpDataChannelBridge::ensure_peer(bool offerer, const std::string& remote_user_id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (peer_) return;
    remote_user_id_ = remote_user_id;

    auto turn = supabase::SupabaseManager::instance().client()->request_turn_credentials();
    if (!turn.success) {
        error_ = turn.error;
        return;
    }

    rtc::Configuration configuration;
    configuration.enableIceTcp = true;
    configuration.maxMessageSize = 64 * 1024;
    for (const auto& server : turn.servers) {
        for (const auto& url : server.urls) {
            try {
                rtc::IceServer ice(url);
                ice.username = server.username;
                ice.password = server.credential;
                configuration.iceServers.push_back(std::move(ice));
            } catch (...) {
                // Ignore malformed provider entries and keep valid servers.
            }
        }
    }
    if (configuration.iceServers.empty()) {
        error_ = "TURN returned no usable ICE server URLs";
        return;
    }

    auto pc = std::make_shared<rtc::PeerConnection>(configuration);
    peer_ = pc;
    pc->onLocalDescription([this](rtc::Description description) {
        Json payload = Json::obj();
        payload.set("sdp", Json::str(description.generateSdp()));
        payload.set("type", Json::str(description.typeString()));
        publish(remote_user_id_, description.type() == rtc::Description::Type::Offer
                                   ? "offer" : "answer", payload.dump());
    });
    pc->onLocalCandidate([this](rtc::Candidate candidate) {
        Json payload = Json::obj();
        payload.set("candidate", Json::str(candidate.candidate()));
        payload.set("mid", Json::str(candidate.mid()));
        publish(remote_user_id_, "candidate", payload.dump());
    });
    pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> channel) {
        install_data_channel(channel);
    });
    pc->onIceStateChange([this](rtc::PeerConnection::IceState state) {
        if (state == rtc::PeerConnection::IceState::Failed) {
            set_error("Direct and TURN ICE connection failed");
            ready_ = false;
        }
    });

    if (offerer) {
        install_data_channel(pc->createDataChannel("amalgam-minecraft"));
        pc->setLocalDescription(rtc::Description::Type::Offer);
    }
}

void TcpDataChannelBridge::install_data_channel(
    const std::shared_ptr<rtc::DataChannel>& channel) {
    if (!channel) return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        channel_ = channel;
    }
    channel->onOpen([this]() { ready_ = true; });
    channel->onClosed([this]() {
        ready_ = false;
        close_socket();
    });
    channel->onError([this](std::string message) {
        set_error("Minecraft data channel failed: " + message);
        ready_ = false;
    });
    channel->onMessage([this](rtc::binary data) {
        SOCKET socket = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stream_socket_ != kInvalidSocket) socket = as_socket(stream_socket_);
        }
        if (socket != INVALID_SOCKET && !send_all(socket, data)) close_socket();
    }, [](rtc::string) {});
}

void TcpDataChannelBridge::handle_signal(const std::string& sender_id,
                                         const std::string& kind,
                                         const std::string& payload) {
    Json message = Json::parse(payload);
    if (!message.isObject()) return;
    if (kind == "hello" && role_ == Role::Host) {
        ensure_peer(true, sender_id);
        return;
    }
    if (sender_id != remote_user_id_) return;
    if (kind == "offer" && role_ == Role::Guest) {
        ensure_peer(false, sender_id);
        std::shared_ptr<rtc::PeerConnection> peer;
        {
            std::lock_guard<std::mutex> lock(mu_);
            peer = peer_;
        }
        if (peer) {
            peer->setRemoteDescription(rtc::Description(
                message.get("sdp").as_str(), message.get("type").as_str("offer")));
            peer->setLocalDescription(rtc::Description::Type::Answer);
        }
    } else if (kind == "answer" && role_ == Role::Host) {
        std::lock_guard<std::mutex> lock(mu_);
        if (peer_) peer_->setRemoteDescription(rtc::Description(
            message.get("sdp").as_str(), message.get("type").as_str("answer")));
    } else if (kind == "candidate") {
        std::lock_guard<std::mutex> lock(mu_);
        if (peer_) peer_->addRemoteCandidate(rtc::Candidate(
            message.get("candidate").as_str(), message.get("mid").as_str()));
    }
}

void TcpDataChannelBridge::poll_signals() {
    std::set<std::string> handled;
    while (running_) {
        auto signals = supabase::SupabaseManager::instance().poll_essentials_signals(
            session_id_, local_user_id_);
        for (const auto& signal : signals) {
            if (!signal.id.empty() && handled.insert(signal.id).second) {
                handle_signal(signal.sender_id, signal.kind, signal.payload);
                supabase::SupabaseManager::instance().remove_essentials_signal(signal.id);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void TcpDataChannelBridge::io_loop() {
    char buffer[16 * 1024];
    while (running_) {
        SOCKET socket = INVALID_SOCKET;
        std::shared_ptr<rtc::DataChannel> channel;
        {
            std::lock_guard<std::mutex> lock(mu_);
            channel = channel_;
            if (stream_socket_ != kInvalidSocket) socket = as_socket(stream_socket_);
        }

        if (channel && channel->isOpen() && socket == INVALID_SOCKET) {
            if (role_ == Role::Host) {
                socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_port = htons(minecraft_port_);
                inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
                if (socket == INVALID_SOCKET ||
                    connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
                    if (socket != INVALID_SOCKET) closesocket(socket);
                    socket = INVALID_SOCKET;
                } else {
                    DWORD timeout = 250;
                    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
                    std::lock_guard<std::mutex> lock(mu_);
                    stream_socket_ = static_cast<uintptr_t>(socket);
                }
            } else {
                SOCKET listener = INVALID_SOCKET;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    if (listener_socket_ != kInvalidSocket) listener = as_socket(listener_socket_);
                }
                if (listener != INVALID_SOCKET) {
                    socket = accept(listener, nullptr, nullptr);
                    if (socket != INVALID_SOCKET) {
                        DWORD timeout = 250;
                        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
                        std::lock_guard<std::mutex> lock(mu_);
                        stream_socket_ = static_cast<uintptr_t>(socket);
                    }
                }
            }
        }

        if (socket != INVALID_SOCKET && channel && channel->isOpen()) {
            const int received = recv(socket, buffer, sizeof(buffer), 0);
            if (received > 0) {
                rtc::binary data(static_cast<size_t>(received));
                std::memcpy(data.data(), buffer, static_cast<size_t>(received));
                if (!channel->send(std::move(data))) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            } else if (received == 0 || (received == SOCKET_ERROR && WSAGetLastError() != WSAETIMEDOUT)) {
                close_socket();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void TcpDataChannelBridge::close_socket() {
    std::lock_guard<std::mutex> lock(mu_);
    if (stream_socket_ != kInvalidSocket) {
        shutdown(as_socket(stream_socket_), SD_BOTH);
        closesocket(as_socket(stream_socket_));
        stream_socket_ = kInvalidSocket;
    }
}

}  // namespace aml::essentials
