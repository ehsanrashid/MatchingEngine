
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

// MatchingEngineServer
class MatchingEngineServer {
private:
    std::shared_ptr<Aeron> aeronClient;
    std::shared_ptr<Publication> respPub, mdPub;
    std::shared_ptr<Subscription> ordSub;
    std::unordered_map<uint32_t, std::unique_ptr<OrderBook>> books;
    std::atomic<bool> running{true};
    std::thread worker;
    
public:
    MatchingEngineServer(int32_t respPubStreamId, int32_t mdPubStreamId, int32_t ordSubStreamId) {
        try {
            Context ctx;
            aeronClient = Aeron::connect(ctx);
            if (!aeronClient) {
                throw std::runtime_error("Failed to connect to Aeron");
            }

            // Add timeout for finding publications/subscriptions
            auto timeout = std::chrono::seconds(5);
            auto start = std::chrono::steady_clock::now();
            
            // Create publications and subscriptions
            auto respPubId = aeronClient->addPublication("aeron:udp?endpoint=localhost:40124", respPubStreamId);
            while (!respPub && std::chrono::steady_clock::now() - start < timeout) {
                respPub = aeronClient->findPublication(respPubId);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!respPub) throw std::runtime_error("Failed to find response publication");

            auto mdPubId = aeronClient->addPublication("aeron:udp?endpoint=localhost:40125", mdPubStreamId);
            start = std::chrono::steady_clock::now();
            while (!mdPub && std::chrono::steady_clock::now() - start < timeout) {
                mdPub = aeronClient->findPublication(mdPubId);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!mdPub) throw std::runtime_error("Failed to find market data publication");
            
            auto ordSubId = aeronClient->addSubscription("aeron:udp?endpoint=localhost:40123", ordSubStreamId);
            start = std::chrono::steady_clock::now();
            while (!ordSub && std::chrono::steady_clock::now() - start < timeout) {
                ordSub = aeronClient->findSubscription(ordSubId);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!ordSub) throw std::runtime_error("Failed to find order subscription");

            // Wait for connections
            while (!respPub->isConnected() || !mdPub->isConnected()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            worker = std::thread(&MatchingEngineServer::run, this);
        } catch (const std::exception& e) {
            std::cerr << "Server initialization error: " << e.what() << std::endl;
            throw;
        }
    }
    
    ~MatchingEngineServer() {
        running = false;
        if (worker.joinable()) {
            worker.join();
        }
    }
    
private:
    void run() {
        const size_t limit = 10;
        while (running) {
            try {
                int fragments = ordSub->poll([this](AtomicBuffer& buf, index_t off, index_t len, const Header& hdr) {
                    try {
                        process(buf, off, len);
                    } catch (const std::exception& e) {
                        std::cerr << "Error processing message: " << e.what() << std::endl;
                    }
                }, limit);
                
                if (fragments == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1));
                }
            } catch (const std::exception& e) {
                std::cerr << "Error in server run loop: " << e.what() << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    
    void process(AtomicBuffer& buf, index_t off, index_t len) {
        if (len < sizeof(MessageType)) return;
        
        const uint8_t* bufPtr = buf.buffer();
        if (!bufPtr) return;
        
        auto type = *reinterpret_cast<const MessageType*>(bufPtr + off);
        
        if (type == MessageType::NEW_ORDER && len >= sizeof(NewOrderMessage)) {
            auto msg = reinterpret_cast<const NewOrderMessage*>(bufPtr + off);
            auto& ob = getBook(msg->symbol);
            Order o{msg->orderId, msg->symbol, msg->side, msg->quantity, msg->price, msg->timestamp};
            auto res = ob.addOrder(o);
            
            MessageType responseType = res.matched ? MessageType::ORDER_FILLED : MessageType::ORDER_ACCEPTED;
            sendResponse(msg->symbol, msg->orderId, res.execQty, res.execPrice, responseType);
            sendMarketData(msg->symbol);
            
        } else if (type == MessageType::CANCEL_ORDER && len >= sizeof(CancelOrderMessage)) {
            auto msg = reinterpret_cast<const CancelOrderMessage*>(bufPtr + off);
            bool cancelled = false;
            uint32_t symbol = 0;
            
            for (auto& [sym, book] : books) {
                if (book->cancel(msg->orderId)) {
                    cancelled = true;
                    symbol = sym;
                    sendMarketData(sym);
                    break;
                }
            }
            
            MessageType responseType = cancelled ? MessageType::ORDER_CANCELLED : MessageType::ORDER_REJECTED;
            sendResponse(symbol, msg->orderId, 0, 0, responseType);
        }
    }
    
    OrderBook& getBook(uint32_t symbol) {
        auto it = books.find(symbol);
        if (it == books.end()) {
            books[symbol] = std::make_unique<OrderBook>();
        }
        return *books[symbol];
    }
    
    void sendResponse(uint32_t symbol, uint64_t orderId, uint64_t quantity, uint64_t price, MessageType type) {
        try {
            OrderResponseMessage resp{type, orderId, symbol, quantity, price, currentTime()};
            AtomicBuffer buf(reinterpret_cast<uint8_t*>(&resp), sizeof(resp));
            
            int attempts = 0;
            while (respPub->offer(buf, 0, sizeof(resp)) < 0 && attempts < 1000) {
                std::this_thread::sleep_for(std::chrono::microseconds(1));
                attempts++;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error sending response: " << e.what() << std::endl;
        }
    }
    
    void sendMarketData(uint32_t symbol) {
        try {
            auto& ob = getBook(symbol);
            auto [bidPrice, bidQty] = ob.bestBid();
            auto [askPrice, askQty] = ob.bestAsk();
            
            MarketDataMessage md{MessageType::MARKET_DATA, symbol, bidPrice, bidQty, askPrice, askQty, currentTime()};
            AtomicBuffer buf(reinterpret_cast<uint8_t*>(&md), sizeof(md));
            
            int attempts = 0;
            while (mdPub->offer(buf, 0, sizeof(md)) < 0 && attempts < 1000) {
                std::this_thread::sleep_for(std::chrono::microseconds(1));
                attempts++;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error sending market data: " << e.what() << std::endl;
        }
    }
    
    uint64_t currentTime() { 
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count(); 
    }
};
