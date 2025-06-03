#pragma once

#include <iostream>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <queue>
#include <unordered_map>
#include <map>
#include <chrono>
#include <cstring>
#include <cstdint>

#include "types.h" // Include the types header for MessageType, Side, Order, etc.
#include "Aeron.h"
#include "Context.h"
#include "Publication.h"
#include "Subscription.h"
#include "concurrent/AtomicBuffer.h"
#include "concurrent/AgentRunner.h"
#include "concurrent/logbuffer/LogBufferDescriptor.h"
#include "concurrent/logbuffer/Header.h"

using namespace aeron;
using namespace aeron::concurrent;
using namespace aeron::concurrent::logbuffer;


// MatchingEngineClient
class MatchingEngineClient {
private:
    std::shared_ptr<Aeron> aeronClient;
    std::shared_ptr<Publication> ordPub;
    std::shared_ptr<Subscription> respSub, mdSub;
    std::atomic<bool> running{true};
    std::thread rThread, mThread;
    
public:
    MatchingEngineClient(int32_t ordSubStreamId, int32_t respPubStreamId, int32_t mdPubStreamId) {
        try {
            Context ctx;
            aeronClient = Aeron::connect(ctx);
            if (!aeronClient) {
                throw std::runtime_error("Failed to connect to Aeron");
            }

            auto timeout = std::chrono::seconds(5);
            auto start = std::chrono::steady_clock::now();
 
            auto ordPubId = aeronClient->addPublication("aeron:udp?endpoint=localhost:40123", ordSubStreamId);
            while (!ordPub && std::chrono::steady_clock::now() - start < timeout) {
                ordPub = aeronClient->findPublication(ordPubId);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!ordPub) throw std::runtime_error("Failed to find order publication");
 
            auto respSubId = aeronClient->addSubscription("aeron:udp?endpoint=localhost:40124", respPubStreamId);
            start = std::chrono::steady_clock::now();
            while (!respSub && std::chrono::steady_clock::now() - start < timeout) {
                respSub = aeronClient->findSubscription(respSubId);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!respSub) throw std::runtime_error("Failed to find response subscription");

            auto mdSubId = aeronClient->addSubscription("aeron:udp?endpoint=localhost:40125", mdPubStreamId);
            start = std::chrono::steady_clock::now();
            while (!mdSub && std::chrono::steady_clock::now() - start < timeout) {
                mdSub = aeronClient->findSubscription(mdSubId);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!mdSub) throw std::runtime_error("Failed to find market data subscription");

            // Wait for order publication to connect
            while (!ordPub->isConnected()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            
            rThread = std::thread(&MatchingEngineClient::recvLoop, this, respSub.get());
            mThread = std::thread(&MatchingEngineClient::recvLoop, this, mdSub.get());
        } catch (const std::exception& e) {
            std::cerr << "Client initialization error: " << e.what() << std::endl;
            throw;
        }
    }
    
    ~MatchingEngineClient() { 
        running = false; 
        if (rThread.joinable()) rThread.join(); 
        if (mThread.joinable()) mThread.join(); 
    }
    // Send order to the server
    void sendOrder(uint64_t id, uint32_t symbol, Side side, uint64_t quantity, uint64_t price) {
        try {
            NewOrderMessage order{MessageType::NEW_ORDER, id, symbol, side, quantity, price, currentTime()};
            AtomicBuffer buf(reinterpret_cast<uint8_t*>(&order), sizeof(order));
            
            int attempts = 0;
            while (ordPub->offer(buf, 0, sizeof(order)) < 0 && attempts < 1000) {
                std::this_thread::sleep_for(std::chrono::microseconds(1));
                attempts++;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error sending order: " << e.what() << std::endl;
        }
    }
    
    void cancelOrder(uint64_t id) {
        try {
            CancelOrderMessage cancel{MessageType::CANCEL_ORDER, id};
            AtomicBuffer buf(reinterpret_cast<uint8_t*>(&cancel), sizeof(cancel));
            
            int attempts = 0;
            while (ordPub->offer(buf, 0, sizeof(cancel)) < 0 && attempts < 1000) {
                std::this_thread::sleep_for(std::chrono::microseconds(1));
                attempts++;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error sending cancel: " << e.what() << std::endl;
        }
    }
    
private:
    void recvLoop(Subscription* sub) {
        while (running) {
            try {
                sub->poll([this](AtomicBuffer& b, index_t off, index_t len, const Header& hdr) {
                    try {
                        processMsg(b, off, len);
                    } catch (const std::exception& e) {
                        std::cerr << "Error processing message: " << e.what() << std::endl;
                    }
                }, 10);
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            } catch (const std::exception& e) {
                std::cerr << "Error in client receive loop: " << e.what() << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    
    void processMsg(AtomicBuffer& buf, index_t off, index_t len) {
        if (len < sizeof(MessageType)) return;
        
        const uint8_t* bufPtr = buf.buffer();
        if (!bufPtr) return;
        
        auto type = *reinterpret_cast<const MessageType*>(bufPtr + off);
        
        switch (type) {
            case MessageType::ORDER_FILLED:
            case MessageType::ORDER_ACCEPTED:
            case MessageType::ORDER_CANCELLED:
            case MessageType::ORDER_REJECTED: {
                if (len >= sizeof(OrderResponseMessage)) {
                    auto resp = reinterpret_cast<const OrderResponseMessage*>(bufPtr + off);
                    std::cout << "Response for order " << resp->orderId << " type=" << static_cast<int>(resp->type) << std::endl;
                }
                break;
            }
            case MessageType::MARKET_DATA: {
                if (len >= sizeof(MarketDataMessage)) {
                    auto md = reinterpret_cast<const MarketDataMessage*>(bufPtr + off);
                    std::cout << "Market Data symbol=" << md->symbol << " bid=" << md->bidPrice 
                              << " ask=" << md->askPrice << std::endl;
                }
                break;
            }
            default: 
                break;
        }
    }
    
    uint64_t currentTime() { 
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count(); 
    }
};
