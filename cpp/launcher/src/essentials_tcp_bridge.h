#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace rtc {
class DataChannel;
class PeerConnection;
}

namespace aml::essentials {

// Bridges one local Minecraft TCP stream to one reliable WebRTC data channel.
// Supabase carries only SDP/ICE signaling; game bytes stay on the data channel.
class TcpDataChannelBridge {
public:
    TcpDataChannelBridge();
    ~TcpDataChannelBridge();

    TcpDataChannelBridge(const TcpDataChannelBridge&) = delete;
    TcpDataChannelBridge& operator=(const TcpDataChannelBridge&) = delete;

    // Host mode connects the data channel to an already-running local server.
    bool start_host(const std::string& session_id, uint16_t minecraft_port);

    // Guest mode listens locally; Minecraft should connect to local_port().
    bool start_guest(const std::string& session_id, const std::string& host_user_id,
                     uint16_t requested_port = 0);

    void stop();
    bool is_ready() const;
    uint16_t local_port() const;
    std::string error() const;

private:
    enum class Role { Host, Guest };

    bool start(Role role, const std::string& session_id, const std::string& host_user_id,
               uint16_t port);
    void poll_signals();
    void ensure_peer(bool offerer, const std::string& remote_user_id);
    void handle_signal(const std::string& sender_id, const std::string& kind,
                       const std::string& payload);
    void publish(const std::string& recipient_id, const std::string& kind,
                 const std::string& payload);
    void install_data_channel(const std::shared_ptr<rtc::DataChannel>& channel);
    void io_loop();
    void close_socket();
    void set_error(const std::string& message);

    Role role_ = Role::Guest;
    std::string session_id_;
    std::string host_user_id_;
    std::string local_user_id_;
    std::string remote_user_id_;
    uint16_t minecraft_port_ = 25565;
    uint16_t local_port_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    mutable std::mutex mu_;
    std::string error_;
    uintptr_t listener_socket_ = static_cast<uintptr_t>(~0ull);
    uintptr_t stream_socket_ = static_cast<uintptr_t>(~0ull);
    std::shared_ptr<rtc::PeerConnection> peer_;
    std::shared_ptr<rtc::DataChannel> channel_;
    std::thread signal_thread_;
    std::thread io_thread_;
};

}  // namespace aml::essentials
