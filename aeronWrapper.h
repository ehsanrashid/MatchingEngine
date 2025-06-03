#pragma once

#include <memory>
#include <string>
#include <functional>
#include <stdexcept>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <vector>
#include <thread>

// Aeron C++ headers
#include "Aeron.h"
#include "concurrent/AgentRunner.h"
#include "concurrent/AtomicBuffer.h"
#include "util/Index.h"

namespace aeron_wrapper {

// Forward declarations
class AeronClient;
class PublicationWrapper;
class SubscriptionWrapper;

// Exception classes
class AeronWrapperException : public std::runtime_error {
public:
    explicit AeronWrapperException(const std::string& message) 
        : std::runtime_error("AeronWrapper: " + message) {}
};

// Publication result enum for better error handling
enum class PublicationResult : int64_t {
    SUCCESS = 1,
    NOT_CONNECTED = -1,
    BACK_PRESSURED = -2,
    ADMIN_ACTION = -3, 
    CLOSED = -4,
    MAX_POSITION_EXCEEDED = -5
};

// Fragment handler with metadata
struct FragmentData {
    const std::uint8_t* buffer;
    std::size_t length;
    std::int64_t position;
    std::int32_t session_id;
    std::int32_t stream_id;
    std::int32_t term_id;
    std::int32_t term_offset;
    
    // Helper to get data as string
    std::string as_string() const {
        return std::string(reinterpret_cast<const char*>(buffer), length);
    }
    
    // Helper to get data as specific type
    template<typename T>
    const T& as() const {
        if (length < sizeof(T)) {
            throw AeronWrapperException("Fragment too small for requested type");
        }
        return *reinterpret_cast<const T*>(buffer);
    }
};

using FragmentHandler = std::function<void(const FragmentData& fragment)>;

// Connection state callback
using ConnectionStateHandler = std::function<void(bool connected)>;

// RAII wrapper for Aeron Client
class AeronClient {
private:
    std::shared_ptr<aeron::Aeron> aeron_;
    std::atomic<bool> running_{false};
    
public:
    // Constructor with optional context configuration
    explicit AeronClient(const std::string& aeron_dir = "") {
        aeron::Context context;
        
        if (!aeron_dir.empty()) {
            context.aeronDir(aeron_dir);
        }
        
        try {
            aeron_ = aeron::Aeron::connect(context);
            running_ = true;
        } catch (const std::exception& e) {
            throw AeronWrapperException("Failed to connect to Aeron: " + std::string(e.what()));
        }
    }
    
    ~AeronClient() {
        close();
    }
    
    // Non-copyable
    AeronClient(const AeronClient&) = delete;
    AeronClient& operator=(const AeronClient&) = delete;
    
    // Movable
    AeronClient(AeronClient&& other) noexcept 
        : aeron_(std::move(other.aeron_))
        , running_(other.running_.load()) {
        other.running_ = false;
    }
    
    AeronClient& operator=(AeronClient&& other) noexcept {
        if (this != &other) {
            close();
            aeron_ = std::move(other.aeron_);
            running_ = other.running_.load();
            other.running_ = false;
        }
        return *this;
    }
    
    void close() {
        if (running_) {
            running_ = false;
            // Close publications and subscriptions handled by Aeron's RAII
            aeron_.reset();
        }
    }
    
    bool is_running() const { return running_; }
    
    // Factory methods
    std::unique_ptr<PublicationWrapper> create_publication(
        const std::string& channel, 
        std::int32_t stream_id,
        const ConnectionStateHandler& connection_handler = nullptr);
        
    std::unique_ptr<SubscriptionWrapper> create_subscription(
        const std::string& channel, 
        std::int32_t stream_id,
        const ConnectionStateHandler& connection_handler = nullptr);
    
    std::shared_ptr<aeron::Aeron> get_aeron() const { return aeron_; }
};

// Publication wrapper with enhanced functionality
class PublicationWrapper {
private:
    std::shared_ptr<aeron::Publication> publication_;
    std::string channel_;
    std::int32_t stream_id_;
    ConnectionStateHandler connection_handler_;
    std::atomic<bool> was_connected_{false};
    
    friend class AeronClient;
    
    PublicationWrapper(
        std::shared_ptr<aeron::Publication> pub, 
        const std::string& channel,
        std::int32_t stream_id,
        const ConnectionStateHandler& handler = nullptr)
        : publication_(std::move(pub))
        , channel_(channel)
        , stream_id_(stream_id)
        , connection_handler_(handler) {}
    
public:
    ~PublicationWrapper() = default;
    
    // Non-copyable but movable
    PublicationWrapper(const PublicationWrapper&) = delete;
    PublicationWrapper& operator=(const PublicationWrapper&) = delete;
    PublicationWrapper(PublicationWrapper&&) = default;
    PublicationWrapper& operator=(PublicationWrapper&&) = default;
    
    // Publishing methods with better error handling
    PublicationResult offer(const std::uint8_t* buffer, std::size_t length) {
        check_connection_state();
        
        if (!publication_) {
            return PublicationResult::CLOSED;
        }
        
        // Create AtomicBuffer from raw pointer and length
        // Note: AtomicBuffer takes non-const pointer, but Aeron doesn't modify during offer
        aeron::concurrent::AtomicBuffer atomic_buffer(
            const_cast<std::uint8_t*>(buffer), 
            static_cast<aeron::util::index_t>(length)
        );
        
        std::int64_t result = publication_->offer(atomic_buffer, 0, static_cast<aeron::util::index_t>(length));
        return static_cast<PublicationResult>(result);
    }
    
    PublicationResult offer(const std::string& message) {
        return offer(reinterpret_cast<const std::uint8_t*>(message.data()), message.size());
    }
    
    template<typename T>
    PublicationResult offer(const T& data) {
        static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
        return offer(reinterpret_cast<const std::uint8_t*>(&data), sizeof(T));
    }
    
    // Offer with retry logic
    PublicationResult offer_with_retry(
        const std::uint8_t* buffer, 
        std::size_t length,
        int max_retries = 3,
        std::chrono::milliseconds retry_delay = std::chrono::milliseconds(1)) {
        
        for (int i = 0; i <= max_retries; ++i) {
            auto result = offer(buffer, length);
            
            if (result == PublicationResult::SUCCESS || 
                result == PublicationResult::CLOSED ||
                result == PublicationResult::NOT_CONNECTED ||
                result == PublicationResult::MAX_POSITION_EXCEEDED) {
                return result;
            }
            
            if (i < max_retries) {
                std::this_thread::sleep_for(retry_delay);
            }
        }
        
        return PublicationResult::BACK_PRESSURED;
    }
    
    PublicationResult offer_with_retry(const std::string& message, int max_retries = 3) {
        return offer_with_retry(
            reinterpret_cast<const std::uint8_t*>(message.data()), 
            message.size(), 
            max_retries);
    }
    
    // Synchronous publish (blocks until success or failure)
    bool publish_sync(
        const std::uint8_t* buffer, 
        std::size_t length,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        
        auto start = std::chrono::steady_clock::now();
        
        while (std::chrono::steady_clock::now() - start < timeout) {
            auto result = offer(buffer, length);
            
            switch (result) {
                case PublicationResult::SUCCESS:
                    return true;
                case PublicationResult::CLOSED:
                case PublicationResult::NOT_CONNECTED:
                case PublicationResult::MAX_POSITION_EXCEEDED:
                    return false;
                case PublicationResult::BACK_PRESSURED:
                case PublicationResult::ADMIN_ACTION:
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                    break;
            }
        }
        
        return false;
    }
    
    bool publish_sync(const std::string& message, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        return publish_sync(reinterpret_cast<const std::uint8_t*>(message.data()), message.size(), timeout);
    }
    
    // Status methods
    bool is_connected() const {
        return publication_ && publication_->isConnected();
    }
    
    bool is_closed() const {
        return !publication_ || publication_->isClosed();
    }
    
    std::int64_t position() const {
        return publication_ ? publication_->position() : -1;
    }
    
    std::int32_t session_id() const {
        return publication_ ? publication_->sessionId() : -1;
    }
    
    std::int32_t stream_id() const { return stream_id_; }
    const std::string& channel() const { return channel_; }
    
    // Get publication constants as string for debugging
    static std::string result_to_string(PublicationResult result) {
        switch (result) {
            case PublicationResult::SUCCESS: return "SUCCESS";
            case PublicationResult::NOT_CONNECTED: return "NOT_CONNECTED";
            case PublicationResult::BACK_PRESSURED: return "BACK_PRESSURED";
            case PublicationResult::ADMIN_ACTION: return "ADMIN_ACTION";
            case PublicationResult::CLOSED: return "CLOSED";
            case PublicationResult::MAX_POSITION_EXCEEDED: return "MAX_POSITION_EXCEEDED";
            default: return "UNKNOWN";
        }
    }
    
private:
    void check_connection_state() {
        if (connection_handler_) {
            bool current_state = is_connected();
            bool previous_state = was_connected_.exchange(current_state);
            
            if (current_state != previous_state) {
                connection_handler_(current_state);
            }
        }
    }
};

// Subscription wrapper with enhanced functionality
class SubscriptionWrapper {
private:
    std::shared_ptr<aeron::Subscription> subscription_;
    std::string channel_;
    std::int32_t stream_id_;
    ConnectionStateHandler connection_handler_;
    std::atomic<bool> was_connected_{false};
    
    friend class AeronClient;
    
    SubscriptionWrapper(
        std::shared_ptr<aeron::Subscription> sub,
        const std::string& channel,
        std::int32_t stream_id,
        const ConnectionStateHandler& handler = nullptr)
        : subscription_(std::move(sub))
        , channel_(channel)
        , stream_id_(stream_id)
        , connection_handler_(handler) {}
    
public:
    ~SubscriptionWrapper() = default;
    
    // Non-copyable but movable
    SubscriptionWrapper(const SubscriptionWrapper&) = delete;
    SubscriptionWrapper& operator=(const SubscriptionWrapper&) = delete;
    SubscriptionWrapper(SubscriptionWrapper&&) = default;
    SubscriptionWrapper& operator=(SubscriptionWrapper&&) = default;
    
    // Polling methods
    int poll(const FragmentHandler& handler, int fragment_limit = 10) {
        check_connection_state();
        
        if (!subscription_) {
            return 0;
        }
        
        return subscription_->poll(
            [&handler](const aeron::concurrent::AtomicBuffer& buffer, aeron::util::index_t offset, 
                      aeron::util::index_t length, const aeron::Header& header) {
                FragmentData fragment{
                    buffer.buffer() + offset,
                    static_cast<std::size_t>(length),
                    header.position(),
                    header.sessionId(),
                    header.streamId(),
                    header.termId(),
                    header.termOffset()
                };
                handler(fragment);
            },
            fragment_limit
        );
    }
    
    // Block poll - polls until at least one message or timeout
    int block_poll(
        const FragmentHandler& handler, 
        std::chrono::milliseconds timeout = std::chrono::milliseconds(1000),
        int fragment_limit = 10) {
        
        auto start = std::chrono::steady_clock::now();
        
        while (std::chrono::steady_clock::now() - start < timeout) {
            int fragments = poll(handler, fragment_limit);
            if (fragments > 0) {
                return fragments;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        
        return 0;
    }
    
    // Continuous polling in background thread
    class BackgroundPoller {
    private:
        std::unique_ptr<std::thread> poll_thread_;
        std::atomic<bool> running_{false};
        
    public:
        BackgroundPoller(SubscriptionWrapper* subscription, const FragmentHandler& handler) {
            running_ = true;
            poll_thread_ = std::make_unique<std::thread>([this, subscription, handler]() {
                while (running_) {
                    try {
                        int fragments = subscription->poll(handler, 10);
                        if (fragments == 0) {
                            std::this_thread::sleep_for(std::chrono::microseconds(100));
                        }
                    } catch (const std::exception&) {
                        // Log error in real implementation
                        break;
                    }
                }
            });
        }
        
        ~BackgroundPoller() {
            stop();
        }
        
        // Non-copyable, non-movable
        BackgroundPoller(const BackgroundPoller&) = delete;
        BackgroundPoller& operator=(const BackgroundPoller&) = delete;
        BackgroundPoller(BackgroundPoller&&) = delete;
        BackgroundPoller& operator=(BackgroundPoller&&) = delete;
        
        void stop() {
            if (running_) {
                running_ = false;
                if (poll_thread_ && poll_thread_->joinable()) {
                    poll_thread_->join();
                }
            }
        }
        
        bool is_running() const { return running_; }
    };
    
    std::unique_ptr<BackgroundPoller> start_background_polling(const FragmentHandler& handler) {
        return std::make_unique<BackgroundPoller>(this, handler);
    }
    
    // Status methods
    bool is_connected() const {
        return subscription_ && subscription_->isConnected();
    }
    
    bool is_closed() const {
        return !subscription_ || subscription_->isClosed();
    }
    
    bool has_images() const {
        return subscription_ && subscription_->imageCount() > 0;
    }
    
    std::int32_t stream_id() const { return stream_id_; }
    const std::string& channel() const { return channel_; }
    
    std::size_t image_count() const {
        return subscription_ ? subscription_->imageCount() : 0;
    }
    
private:
    void check_connection_state() {
        if (connection_handler_) {
            bool current_state = is_connected();
            bool previous_state = was_connected_.exchange(current_state);
            
            if (current_state != previous_state) {
                connection_handler_(current_state);
            }
        }
    }
};

// Implementation of AeronClient factory methods
inline std::unique_ptr<PublicationWrapper> AeronClient::create_publication(
    const std::string& channel, 
    std::int32_t stream_id,
    const ConnectionStateHandler& connection_handler) {
    
    if (!running_) {
        throw AeronWrapperException("AeronClient is not running");
    }
    
    try {
        auto publicationId = aeron_->addPublication(channel, stream_id);
        
        // Poll for publication to become available
        std::shared_ptr<aeron::Publication> publication;
        auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        
        while (std::chrono::steady_clock::now() < timeout) {
            publication = aeron_->findPublication(publicationId);
            if (publication) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        
        if (!publication) {
            throw AeronWrapperException("Failed to find publication with ID: " + std::to_string(publicationId));
        }
        
        // Wait for publication to be ready (with timeout)
        while (!publication->isConnected() && !publication->isClosed() && 
               std::chrono::steady_clock::now() < timeout) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        
        return std::unique_ptr<PublicationWrapper>(
            new PublicationWrapper(std::move(publication), channel, stream_id, connection_handler));
            
    } catch (const std::exception& e) {
        throw AeronWrapperException("Failed to create publication: " + std::string(e.what()));
    }
}

inline std::unique_ptr<SubscriptionWrapper> AeronClient::create_subscription(
    const std::string& channel, 
    std::int32_t stream_id,
    const ConnectionStateHandler& connection_handler) {
    
    if (!running_) {
        throw AeronWrapperException("AeronClient is not running");
    }
    
    try {
        auto subscriptionId = aeron_->addSubscription(channel, stream_id);
        
        // Poll for subscription to become available
        std::shared_ptr<aeron::Subscription> subscription;
        auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        
        while (std::chrono::steady_clock::now() < timeout) {
            subscription = aeron_->findSubscription(subscriptionId);
            if (subscription) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        
        if (!subscription) {
            throw AeronWrapperException("Failed to find subscription with ID: " + std::to_string(subscriptionId));
        }
        
        // Wait for subscription to be ready (with timeout)
        while (!subscription->isConnected() && !subscription->isClosed() && 
               std::chrono::steady_clock::now() < timeout) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        
        return std::unique_ptr<SubscriptionWrapper>(
            new SubscriptionWrapper(std::move(subscription), channel, stream_id, connection_handler));
            
    } catch (const std::exception& e) {
        throw AeronWrapperException("Failed to create subscription: " + std::string(e.what()));
    }
}

} // namespace aeron_wrapper

// Example usage:
/*
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    try {
        // Create Aeron client
        aeron_wrapper::AeronClient client("/tmp/aeron");
        
        // Connection state handlers
        auto connection_handler = [](bool connected) {
            std::cout << "Connection state changed: " << (connected ? "Connected" : "Disconnected") << std::endl;
        };
        
        // Create publication and subscription
        auto pub = client.create_publication("aeron:ipc", 1001, connection_handler);
        auto sub = client.create_subscription("aeron:ipc", 1001, connection_handler);
        
        // Wait for connections
        while (!pub->is_connected() || !sub->is_connected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        std::cout << "Publication and Subscription connected!" << std::endl;
        
        // Start background polling
        auto background_poller = sub->start_background_polling([](const aeron_wrapper::FragmentData& fragment) {
            std::cout << "Received: " << fragment.as_string() 
                     << " (position: " << fragment.position << ")" << std::endl;
        });
        
        // Publish messages
        for (int i = 0; i < 10; ++i) {
            std::string message = "Hello World " + std::to_string(i);
            
            auto result = pub->offer_with_retry(message);
            std::cout << "Published '" << message << "' - Result: " 
                     << aeron_wrapper::PublicationWrapper::result_to_string(result) << std::endl;
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        // Wait for messages to be processed
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        // Stop background polling
        background_poller->stop();
        
        std::cout << "Example completed successfully!" << std::endl;
        
    } catch (const aeron_wrapper::AeronWrapperException& e) {
        std::cerr << "Aeron wrapper error: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
*/