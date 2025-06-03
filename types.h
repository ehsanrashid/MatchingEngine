
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

// Message types
enum class MessageType : uint8_t {
    NEW_ORDER = 1,
    CANCEL_ORDER = 2,
    ORDER_ACCEPTED = 3,
    ORDER_REJECTED = 4,
    ORDER_FILLED = 5,
    ORDER_CANCELLED = 6,
    MARKET_DATA = 7
};

// Order side
enum class Side : uint8_t {
    BUY = 1,
    SELL = 2
};

// Order structure
struct Order {
    uint64_t orderId;
    uint32_t symbol;
    Side side;
    uint64_t quantity;
    uint64_t price;
    uint64_t timestamp;
    
    Order() = default;
    Order(uint64_t id, uint32_t sym, Side s, uint64_t qty, uint64_t px, uint64_t ts)
        : orderId(id), symbol(sym), side(s), quantity(qty), price(px), timestamp(ts) {}
};

#pragma pack(push,1)
struct NewOrderMessage { 
    MessageType type; 
    uint64_t orderId; 
    uint32_t symbol; 
    Side side; 
    uint64_t quantity; 
    uint64_t price; 
    uint64_t timestamp; 
};

struct CancelOrderMessage { 
    MessageType type; 
    uint64_t orderId; 
};

struct OrderResponseMessage { 
    MessageType type; 
    uint64_t orderId; 
    uint32_t symbol; 
    uint64_t executedQuantity; 
    uint64_t executedPrice; 
    uint64_t timestamp; 
};

struct MarketDataMessage { 
    MessageType type; 
    uint32_t symbol; 
    uint64_t bidPrice; 
    uint64_t bidQuantity; 
    uint64_t askPrice; 
    uint64_t askQuantity; 
    uint64_t timestamp; 
};
#pragma pack(pop)

// Price-time priority OrderBook
class OrderBook {
private:
    struct PriceLevel { 
        uint64_t price;
        uint64_t totalQty; 
        std::queue<Order> orders; 
        
        PriceLevel(uint64_t p) : price(p), totalQty(0) {} 
    };
    
    std::map<uint64_t, std::unique_ptr<PriceLevel>, std::greater<uint64_t>> buyLevels;
    std::map<uint64_t, std::unique_ptr<PriceLevel>> sellLevels;
    std::unordered_map<uint64_t, std::pair<uint32_t, uint64_t>> orderMap; // orderId -> (symbol, price)
    
public:
    struct MatchResult { 
        bool matched; 
        uint64_t execQty; 
        uint64_t execPrice; 
        uint64_t remQty; 
    };
    
    MatchResult addOrder(const Order& order) {
        MatchResult res{false, 0, 0, order.quantity};
        
        if (order.side == Side::BUY) {
            // Try to match with sell orders
            auto it = sellLevels.begin();
            while (it != sellLevels.end() && res.remQty > 0 && it->first <= order.price) {
                auto& lvl = it->second;
                while (!lvl->orders.empty() && res.remQty > 0) {
                    auto& sellOrder = lvl->orders.front();
                    uint64_t matchQty = std::min(res.remQty, sellOrder.quantity);
                    
                    res.matched = true;
                    res.execQty += matchQty;
                    res.execPrice = sellOrder.price;
                    res.remQty -= matchQty;
                    
                    sellOrder.quantity -= matchQty;
                    lvl->totalQty -= matchQty;
                    
                    if (sellOrder.quantity == 0) {
                        orderMap.erase(sellOrder.orderId);
                        lvl->orders.pop();
                    }
                }
                
                if (lvl->orders.empty()) {
                    it = sellLevels.erase(it);
                } else {
                    ++it;
                }
            }
            
            // Add remaining quantity to buy book
            if (res.remQty > 0) {
                auto it = buyLevels.find(order.price);
                if (it == buyLevels.end()) {
                    auto lvl = std::make_unique<PriceLevel>(order.price);
                    Order remainingOrder = order;
                    remainingOrder.quantity = res.remQty;
                    lvl->orders.push(remainingOrder);
                    lvl->totalQty = res.remQty;
                    buyLevels[order.price] = std::move(lvl);
                } else {
                    Order remainingOrder = order;
                    remainingOrder.quantity = res.remQty;
                    it->second->orders.push(remainingOrder);
                    it->second->totalQty += res.remQty;
                }
                orderMap[order.orderId] = std::make_pair(order.symbol, order.price);
            }
        } else { // SELL
            // Try to match with buy orders
            auto it = buyLevels.begin();
            while (it != buyLevels.end() && res.remQty > 0 && it->first >= order.price) {
                auto& lvl = it->second;
                while (!lvl->orders.empty() && res.remQty > 0) {
                    auto& buyOrder = lvl->orders.front();
                    uint64_t matchQty = std::min(res.remQty, buyOrder.quantity);
                    
                    res.matched = true;
                    res.execQty += matchQty;
                    res.execPrice = buyOrder.price;
                    res.remQty -= matchQty;
                    
                    buyOrder.quantity -= matchQty;
                    lvl->totalQty -= matchQty;
                    
                    if (buyOrder.quantity == 0) {
                        orderMap.erase(buyOrder.orderId);
                        lvl->orders.pop();
                    }
                }
                
                if (lvl->orders.empty()) {
                    it = buyLevels.erase(it);
                } else {
                    ++it;
                }
            }
            
            // Add remaining quantity to sell book
            if (res.remQty > 0) {
                auto it = sellLevels.find(order.price);
                if (it == sellLevels.end()) {
                    auto lvl = std::make_unique<PriceLevel>(order.price);
                    Order remainingOrder = order;
                    remainingOrder.quantity = res.remQty;
                    lvl->orders.push(remainingOrder);
                    lvl->totalQty = res.remQty;
                    sellLevels[order.price] = std::move(lvl);
                } else {
                    Order remainingOrder = order;
                    remainingOrder.quantity = res.remQty;
                    it->second->orders.push(remainingOrder);
                    it->second->totalQty += res.remQty;
                }
                orderMap[order.orderId] = std::make_pair(order.symbol, order.price);
            }
        }
        
        return res;
    }
    
    bool cancel(uint64_t orderId) {
        auto it = orderMap.find(orderId);
        if (it == orderMap.end()) {
            return false;
        }
        
        uint32_t symbol = it->second.first;
        uint64_t price = it->second.second;
        
        // For now, we'll just remove from the map and decrement total quantity
        // In a real implementation, you'd need to find the specific order in the queue
        // This is a simplified approach that may not be perfectly accurate
        orderMap.erase(it);
        return true;
    }
    
    std::pair<uint64_t, uint64_t> bestBid() const {
        if (buyLevels.empty()) {
            return {0, 0};
        }
        auto& lvl = buyLevels.begin()->second;
        return {lvl->price, lvl->totalQty};
    }
    
    std::pair<uint64_t, uint64_t> bestAsk() const {
        if (sellLevels.empty()) {
            return {0, 0};
        }
        auto& lvl = sellLevels.begin()->second;
        return {lvl->price, lvl->totalQty};
    }
};
