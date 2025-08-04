#pragma once

#include <unordered_map>
#include <map>
#include <queue>
#include <string>
#include <memory>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <iostream>

// Include the Aeron wrapper
#include "aeronWrapper.h" // Uncomment when using

namespace matching_engine {

// Order types
enum class OrderType : std::uint8_t {
    MARKET = 0,
    LIMIT = 1,
    STOP = 2,
    STOP_LIMIT = 3
};

enum class OrderSide : std::uint8_t {
    BUY = 0,
    SELL = 1
};

enum class OrderStatus : std::uint8_t {
    NEW = 0,
    PARTIALLY_FILLED = 1,
    FILLED = 2,
    CANCELLED = 3,
    REJECTED = 4,
    EXPIRED = 5
};

enum class MessageType : std::uint8_t {
    NEW_ORDER = 1,
    CANCEL_ORDER = 2,
    MODIFY_ORDER = 3,
    ORDER_ACK = 10,
    ORDER_REJECT = 11,
    FILL_REPORT = 12,
    CANCEL_ACK = 13,
    MARKET_DATA = 20
};

// Message structures for wire protocol
#pragma pack(push, 1)

struct MessageHeader {
    MessageType msg_type;
    std::uint32_t msg_length;
    std::uint64_t timestamp;
    std::uint64_t sequence_number;
};

struct NewOrderMessage {
    MessageHeader header;
    std::uint64_t order_id;
    char symbol[16];
    OrderSide side;
    OrderType type;
    std::uint64_t quantity;
    std::uint64_t price;  // In ticks (e.g., cents)
    std::uint32_t client_id;
    char client_order_id[32];
};

struct CancelOrderMessage {
    MessageHeader header;
    std::uint64_t order_id;
    std::uint32_t client_id;
    char orig_client_order_id[32];
};

struct OrderAckMessage {
    MessageHeader header;
    std::uint64_t order_id;
    OrderStatus status;
    char symbol[16];
    std::uint32_t client_id;
    char client_order_id[32];
};

struct FillReportMessage {
    MessageHeader header;
    std::uint64_t order_id;
    std::uint64_t fill_id;
    char symbol[16];
    OrderSide side;
    std::uint64_t fill_quantity;
    std::uint64_t fill_price;
    std::uint64_t leaves_quantity;
    std::uint32_t client_id;
    char client_order_id[32];
    std::uint64_t counterparty_order_id;
};

struct MarketDataMessage {
    MessageHeader header;
    char symbol[16];
    std::uint64_t bid_price;
    std::uint64_t bid_quantity;
    std::uint64_t ask_price;
    std::uint64_t ask_quantity;
    std::uint64_t last_price;
    std::uint64_t last_quantity;
    std::uint64_t total_volume;
    std::uint32_t trade_count;
};

#pragma pack(pop)

// Order representation
struct Order {
    std::uint64_t order_id;
    std::string symbol;
    OrderSide side;
    OrderType type;
    std::uint64_t original_quantity;
    std::uint64_t remaining_quantity;
    std::uint64_t price;
    std::uint32_t client_id;
    std::string client_order_id;
    std::chrono::high_resolution_clock::time_point timestamp;
    OrderStatus status;
    
    Order() = default;
    
    Order(std::uint64_t id, const std::string& sym, OrderSide s, OrderType t,
          std::uint64_t qty, std::uint64_t p, std::uint32_t client, const std::string& client_ord_id)
        : order_id(id), symbol(sym), side(s), type(t), original_quantity(qty)
        , remaining_quantity(qty), price(p), client_id(client), client_order_id(client_ord_id)
        , timestamp(std::chrono::high_resolution_clock::now()), status(OrderStatus::NEW) {}
    
    bool is_buy() const { return side == OrderSide::BUY; }
    bool is_sell() const { return side == OrderSide::SELL; }
    bool is_filled() const { return remaining_quantity == 0; }
    std::uint64_t filled_quantity() const { return original_quantity - remaining_quantity; }
};

// Price level for order book
struct PriceLevel {
    std::uint64_t price;
    std::uint64_t total_quantity;
    std::queue<std::shared_ptr<Order>> orders;
    
    PriceLevel(std::uint64_t p) : price(p), total_quantity(0) {}
    
    void add_order(std::shared_ptr<Order> order) {
        orders.push(order);
        total_quantity += order->remaining_quantity;
    }
    
    void remove_quantity(std::uint64_t qty) {
        total_quantity = (total_quantity >= qty) ? total_quantity - qty : 0;
    }
    
    bool is_empty() const {
        return orders.empty() || total_quantity == 0;
    }
};

// Order book for a single symbol
class OrderBook {
private:
    std::string symbol_;
    
    // Buy orders sorted by price (highest first)
    std::map<std::uint64_t, std::unique_ptr<PriceLevel>, std::greater<std::uint64_t>> bid_levels_;
    
    // Sell orders sorted by price (lowest first)
    std::map<std::uint64_t, std::unique_ptr<PriceLevel>, std::less<std::uint64_t>> ask_levels_;
    
    std::unordered_map<std::uint64_t, std::shared_ptr<Order>> orders_;
    
    // Market data tracking
    std::uint64_t last_trade_price_ = 0;
    std::uint64_t last_trade_quantity_ = 0;
    std::uint64_t total_volume_ = 0;
    std::uint32_t trade_count_ = 0;
    
    mutable std::mutex book_mutex_;
    
public:
    explicit OrderBook(const std::string& symbol) : symbol_(symbol) {}
    
    struct Trade {
        std::uint64_t price;
        std::uint64_t quantity;
        std::shared_ptr<Order> aggressive_order;
        std::shared_ptr<Order> passive_order;
    };
    
    // Add order to book
    std::vector<Trade> add_order(std::shared_ptr<Order> order) {
        std::lock_guard<std::mutex> lock(book_mutex_);
        std::vector<Trade> trades;
        
        orders_[order->order_id] = order;
        
        if (order->type == OrderType::MARKET) {
            trades = execute_market_order(order);
        } else if (order->type == OrderType::LIMIT) {
            trades = execute_limit_order(order);
        }
        
        return trades;
    }
    
    // Cancel order - FIXED: Properly remove order from price levels
    bool cancel_order(std::uint64_t order_id) {
        std::lock_guard<std::mutex> lock(book_mutex_);
        
        auto it = orders_.find(order_id);
        if (it == orders_.end()) {
            return false;
        }
        
        auto order = it->second;
        if (order->status != OrderStatus::NEW && order->status != OrderStatus::PARTIALLY_FILLED) {
            return false;
        }
        
        remove_order_from_book(order);
        order->status = OrderStatus::CANCELLED;
        
        return true;
    }
    
    // Get market data snapshot
    MarketDataMessage get_market_data() const {
        std::lock_guard<std::mutex> lock(book_mutex_);
        
        MarketDataMessage md = {};
        md.header.msg_type = MessageType::MARKET_DATA;
        md.header.msg_length = sizeof(MarketDataMessage);
        md.header.timestamp = get_timestamp();
        
        std::strncpy(md.symbol, symbol_.c_str(), sizeof(md.symbol) - 1);
        
        // Best bid
        if (!bid_levels_.empty()) {
            const auto& best_bid = bid_levels_.begin()->second;
            md.bid_price = best_bid->price;
            md.bid_quantity = best_bid->total_quantity;
        }
        
        // Best ask
        if (!ask_levels_.empty()) {
            const auto& best_ask = ask_levels_.begin()->second;
            md.ask_price = best_ask->price;
            md.ask_quantity = best_ask->total_quantity;
        }
        
        md.last_price = last_trade_price_;
        md.last_quantity = last_trade_quantity_;
        md.total_volume = total_volume_;
        md.trade_count = trade_count_;
        
        return md;
    }
    
    // Get order by ID
    std::shared_ptr<Order> get_order(std::uint64_t order_id) {
        std::lock_guard<std::mutex> lock(book_mutex_);
        auto it = orders_.find(order_id);
        return (it != orders_.end()) ? it->second : nullptr;
    }
    
    std::string get_symbol() const { return symbol_; }
    
private:
    std::vector<Trade> execute_market_order(std::shared_ptr<Order> order) {
        std::vector<Trade> trades;
        
        if (order->is_buy()) {
            while (!ask_levels_.empty() && order->remaining_quantity > 0) {
                auto level_it = ask_levels_.begin();
                auto& level = level_it->second;
                
                if (level->is_empty()) {
                    ask_levels_.erase(level_it);
                    continue;
                }
                
                while (!level->orders.empty() && order->remaining_quantity > 0) {
                    auto passive_order = level->orders.front();
                    level->orders.pop();
                    
                    if (passive_order->remaining_quantity == 0) {
                        continue;
                    }
                    
                    std::uint64_t trade_quantity = std::min(order->remaining_quantity, 
                                                        passive_order->remaining_quantity);
                    
                    Trade trade;
                    trade.price = passive_order->price;
                    trade.quantity = trade_quantity;
                    trade.aggressive_order = order;
                    trade.passive_order = passive_order;
                    trades.push_back(trade);
                    
                    // Update quantities
                    order->remaining_quantity -= trade_quantity;
                    passive_order->remaining_quantity -= trade_quantity;
                    level->remove_quantity(trade_quantity);
                    
                    // Update order statuses
                    if (order->is_filled()) {
                        order->status = OrderStatus::FILLED;
                    } else {
                        order->status = OrderStatus::PARTIALLY_FILLED;
                    }
                    
                    if (passive_order->is_filled()) {
                        passive_order->status = OrderStatus::FILLED;
                    } else {
                        passive_order->status = OrderStatus::PARTIALLY_FILLED;
                        // Put partially filled order back
                        level->orders.push(passive_order);
                        level->total_quantity += passive_order->remaining_quantity;
                    }
                    
                    // Update market data
                    last_trade_price_ = trade.price;
                    last_trade_quantity_ = trade.quantity;
                    total_volume_ += trade.quantity;
                    trade_count_++;
                }
                
                if (level->is_empty()) {
                    ask_levels_.erase(level_it);
                }
            }
        } else {
            while (!bid_levels_.empty() && order->remaining_quantity > 0) {
                auto level_it = bid_levels_.begin();
                auto& level = level_it->second;
                
                if (level->is_empty()) {
                    bid_levels_.erase(level_it);
                    continue;
                }
                
                while (!level->orders.empty() && order->remaining_quantity > 0) {
                    auto passive_order = level->orders.front();
                    level->orders.pop();
                    
                    if (passive_order->remaining_quantity == 0) {
                        continue;
                    }
                    
                    std::uint64_t trade_quantity = std::min(order->remaining_quantity, 
                                                        passive_order->remaining_quantity);
                    
                    Trade trade;
                    trade.price = passive_order->price;
                    trade.quantity = trade_quantity;
                    trade.aggressive_order = order;
                    trade.passive_order = passive_order;
                    trades.push_back(trade);
                    
                    // Update quantities
                    order->remaining_quantity -= trade_quantity;
                    passive_order->remaining_quantity -= trade_quantity;
                    level->remove_quantity(trade_quantity);
                    
                    // Update order statuses
                    if (order->is_filled()) {
                        order->status = OrderStatus::FILLED;
                    } else {
                        order->status = OrderStatus::PARTIALLY_FILLED;
                    }
                    
                    if (passive_order->is_filled()) {
                        passive_order->status = OrderStatus::FILLED;
                    } else {
                        passive_order->status = OrderStatus::PARTIALLY_FILLED;
                        // Put partially filled order back
                        level->orders.push(passive_order);
                        level->total_quantity += passive_order->remaining_quantity;
                    }
                    
                    // Update market data
                    last_trade_price_ = trade.price;
                    last_trade_quantity_ = trade.quantity;
                    total_volume_ += trade.quantity;
                    trade_count_++;
                }
                
                if (level->is_empty()) {
                    bid_levels_.erase(level_it);
                }
            }
        }

        // If market order not fully filled, reject remaining quantity
        if (order->remaining_quantity > 0) {
            order->status = (order->filled_quantity() > 0) ? 
                OrderStatus::PARTIALLY_FILLED : OrderStatus::REJECTED;
        }
        
        return trades;
    }
    
    std::vector<Trade> execute_limit_order(std::shared_ptr<Order> order) {
        std::vector<Trade> trades;
        
        // First try to match against existing orders
        if (order->is_buy()) {
            for (auto level_it = ask_levels_.begin(); 
                level_it != ask_levels_.end() && order->remaining_quantity > 0;) {
                
                auto& level = level_it->second;
                
                // Check if we can trade at this price level
                if (order->price < level->price) {
                    break;
                }
                
                while (!level->orders.empty() && order->remaining_quantity > 0) {
                    auto passive_order = level->orders.front();
                    level->orders.pop();
                    
                    if (passive_order->remaining_quantity == 0) {
                        continue;
                    }
                    
                    std::uint64_t trade_quantity = std::min(order->remaining_quantity, 
                                                        passive_order->remaining_quantity);
                    
                    Trade trade;
                    trade.price = passive_order->price;
                    trade.quantity = trade_quantity;
                    trade.aggressive_order = order;
                    trade.passive_order = passive_order;
                    trades.push_back(trade);
                    
                    // Update quantities
                    order->remaining_quantity -= trade_quantity;
                    passive_order->remaining_quantity -= trade_quantity;
                    level->remove_quantity(trade_quantity);
                    
                    // Update order statuses
                    if (order->is_filled()) {
                        order->status = OrderStatus::FILLED;
                    } else {
                        order->status = OrderStatus::PARTIALLY_FILLED;
                    }
                    
                    if (passive_order->is_filled()) {
                        passive_order->status = OrderStatus::FILLED;
                    } else {
                        passive_order->status = OrderStatus::PARTIALLY_FILLED;
                        level->orders.push(passive_order);
                        level->total_quantity += passive_order->remaining_quantity;
                    }
                    
                    // Update market data
                    last_trade_price_ = trade.price;
                    last_trade_quantity_ = trade.quantity;
                    total_volume_ += trade.quantity;
                    trade_count_++;
                }
                
                if (level->is_empty()) {
                    level_it = ask_levels_.erase(level_it);
                } else {
                    ++level_it;
                }
            }
        } else {
            for (auto level_it = bid_levels_.begin(); 
                level_it != bid_levels_.end() && order->remaining_quantity > 0;) {
                
                auto& level = level_it->second;
                
                // Check if we can trade at this price level
                if (order->price > level->price) {
                    break;
                }
                
                while (!level->orders.empty() && order->remaining_quantity > 0) {
                    auto passive_order = level->orders.front();
                    level->orders.pop();
                    
                    if (passive_order->remaining_quantity == 0) {
                        continue;
                    }
                    
                    std::uint64_t trade_quantity = std::min(order->remaining_quantity, 
                                                        passive_order->remaining_quantity);
                    
                    Trade trade;
                    trade.price = passive_order->price;
                    trade.quantity = trade_quantity;
                    trade.aggressive_order = order;
                    trade.passive_order = passive_order;
                    trades.push_back(trade);
                    
                    // Update quantities
                    order->remaining_quantity -= trade_quantity;
                    passive_order->remaining_quantity -= trade_quantity;
                    level->remove_quantity(trade_quantity);
                    
                    // Update order statuses
                    if (order->is_filled()) {
                        order->status = OrderStatus::FILLED;
                    } else {
                        order->status = OrderStatus::PARTIALLY_FILLED;
                    }
                    
                    if (passive_order->is_filled()) {
                        passive_order->status = OrderStatus::FILLED;
                    } else {
                        passive_order->status = OrderStatus::PARTIALLY_FILLED;
                        level->orders.push(passive_order);
                        level->total_quantity += passive_order->remaining_quantity;
                    }
                    
                    // Update market data
                    last_trade_price_ = trade.price;
                    last_trade_quantity_ = trade.quantity;
                    total_volume_ += trade.quantity;
                    trade_count_++;
                }
                
                if (level->is_empty()) {
                    level_it = bid_levels_.erase(level_it);
                } else {
                    ++level_it;
                }
            }
        }

        // Add remaining quantity to book if any
        if (order->remaining_quantity > 0) {
            add_order_to_book(order);
        }
        
        return trades;
    }
    
    void add_order_to_book(std::shared_ptr<Order> order) {
        if (order->is_buy()) {
            auto level_it = bid_levels_.find(order->price);
            if (level_it == bid_levels_.end()) {
                auto new_level = std::make_unique<PriceLevel>(order->price);
                new_level->add_order(order);
                bid_levels_[order->price] = std::move(new_level);
            } else {
                level_it->second->add_order(order);
            }
        } else {
            auto level_it = ask_levels_.find(order->price);
            if (level_it == ask_levels_.end()) {
                auto new_level = std::make_unique<PriceLevel>(order->price);
                new_level->add_order(order);
                ask_levels_[order->price] = std::move(new_level);
            } else {
                level_it->second->add_order(order);
            }
        }
    }
    
    // FIXED: Properly remove orders from queues during cancellation
    void remove_order_from_book(std::shared_ptr<Order> order) {
        if (order->is_buy()) {
            auto level_it = bid_levels_.find(order->price);
            if (level_it != bid_levels_.end()) {
                auto& level = level_it->second;
                level->remove_quantity(order->remaining_quantity);
                
                // Remove the order from the queue (this is a simplified approach)
                // In a real implementation, you'd want to use a more efficient structure
                std::queue<std::shared_ptr<Order>> new_queue;
                while (!level->orders.empty()) {
                    auto current_order = level->orders.front();
                    level->orders.pop();
                    if (current_order->order_id != order->order_id) {
                        new_queue.push(current_order);
                    }
                }
                level->orders = std::move(new_queue);
                
                if (level->is_empty()) {
                    bid_levels_.erase(level_it);
                }
            }
        } else {
            auto level_it = ask_levels_.find(order->price);
            if (level_it != ask_levels_.end()) {
                auto& level = level_it->second;
                level->remove_quantity(order->remaining_quantity);
                
                // Remove the order from the queue
                std::queue<std::shared_ptr<Order>> new_queue;
                while (!level->orders.empty()) {
                    auto current_order = level->orders.front();
                    level->orders.pop();
                    if (current_order->order_id != order->order_id) {
                        new_queue.push(current_order);
                    }
                }
                level->orders = std::move(new_queue);
                
                if (level->is_empty()) {
                    ask_levels_.erase(level_it);
                }
            }
        }
    }
    
    static std::uint64_t get_timestamp() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }
};

// Main matching engine
class MatchingEngine {
private:
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> order_books_;
    std::atomic<std::uint64_t> next_order_id_{1};
    std::atomic<std::uint64_t> next_fill_id_{1};
    std::atomic<std::uint64_t> sequence_number_{1};
    
    // Aeron client and publishers
    std::unique_ptr<aeron_wrapper::AeronClient> aeron_client_;
    std::unique_ptr<aeron_wrapper::PublicationWrapper> order_response_publisher_;
    std::unique_ptr<aeron_wrapper::PublicationWrapper> market_data_publisher_;
    std::unique_ptr<aeron_wrapper::SubscriptionWrapper> order_subscriber_;
    
    // Background threads
    std::unique_ptr<std::thread> order_processing_thread_;
    std::unique_ptr<std::thread> market_data_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> aeron_connected_{false};
    std::atomic<bool> shutdown_requested_{false};
    
    mutable std::mutex engine_mutex_;
    mutable std::mutex aeron_mutex_;  // Separate mutex for Aeron operations
    
    // Configuration
    static constexpr int CONNECTION_TIMEOUT_MS = 15000;  // Increased timeout
    static constexpr int CONNECTION_CHECK_INTERVAL_MS = 50;  // More frequent checks
    static constexpr int MAX_CONNECTION_RETRIES = 3;
    
public:
    explicit MatchingEngine(const std::string& aeron_dir = "") {
        std::cout << "Initializing MatchingEngine..." << std::endl;
        
        // Initialize Aeron with retry logic
        bool aeron_initialized = initialize_aeron_with_retry(aeron_dir, MAX_CONNECTION_RETRIES);
        
        if (!aeron_initialized) {
            std::cerr << "Warning: Failed to initialize Aeron after " << MAX_CONNECTION_RETRIES 
                     << " attempts. Starting engine in standalone mode." << std::endl;
            aeron_connected_ = false;
        }
        
        running_ = true;
        
        // Start background threads only if Aeron is connected
        if (aeron_connected_) {
            std::cout << "Starting background threads..." << std::endl;
            order_processing_thread_ = std::make_unique<std::thread>(&MatchingEngine::process_orders, this);
            market_data_thread_ = std::make_unique<std::thread>(&MatchingEngine::publish_market_data, this);
        } else {
            std::cout << "Running in standalone mode - no background threads started." << std::endl;
        }
    }
    
    ~MatchingEngine() {
        stop();
    }
    
    void stop() {
        if (running_) {
            std::cout << "Stopping MatchingEngine..." << std::endl;
            shutdown_requested_ = true;
            running_ = false;
            
            // Wait for threads to finish
            if (order_processing_thread_ && order_processing_thread_->joinable()) {
                std::cout << "Waiting for order processing thread to stop..." << std::endl;
                order_processing_thread_->join();
            }
            
            if (market_data_thread_ && market_data_thread_->joinable()) {
                std::cout << "Waiting for market data thread to stop..." << std::endl;
                market_data_thread_->join();
            }
            
            // Clean shutdown of Aeron resources
            {
                std::lock_guard<std::mutex> lock(aeron_mutex_);
                order_subscriber_.reset();
                order_response_publisher_.reset();
                market_data_publisher_.reset();
                aeron_client_.reset();
            }
            
            std::cout << "MatchingEngine stopped." << std::endl;
        }
    }
    
    // Add a new symbol to the engine
    void add_symbol(const std::string& symbol) {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        if (order_books_.find(symbol) == order_books_.end()) {
            order_books_[symbol] = std::make_unique<OrderBook>(symbol);
            std::cout << "Added symbol: " << symbol << std::endl;
        }
    }
    
    // Direct order processing (for standalone mode or testing)
    std::vector<OrderBook::Trade> process_order_direct(const std::string& symbol, 
                                                      OrderSide side, OrderType type,
                                                      std::uint64_t quantity, std::uint64_t price,
                                                      std::uint32_t client_id, 
                                                      const std::string& client_order_id = "") {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        
        // Get or create order book
        auto it = order_books_.find(symbol);
        if (it == order_books_.end()) {
            order_books_[symbol] = std::make_unique<OrderBook>(symbol);
            it = order_books_.find(symbol);
        }
        
        // Create order
        std::uint64_t order_id = next_order_id_++;
        std::string final_client_order_id = client_order_id.empty() ? 
            ("ORDER_" + std::to_string(order_id)) : client_order_id;
        
        auto order = std::make_shared<Order>(
            order_id, symbol, side, type, quantity, price, client_id, final_client_order_id);
        
        // Process order
        return it->second->add_order(order);
    }
    
    // Cancel order directly
    bool cancel_order_direct(const std::string& symbol, std::uint64_t order_id) {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        
        auto it = order_books_.find(symbol);
        if (it != order_books_.end()) {
            return it->second->cancel_order(order_id);
        }
        return false;
    }
    
    // Get market data for a symbol
    MarketDataMessage get_market_data(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        
        auto it = order_books_.find(symbol);
        if (it != order_books_.end()) {
            return it->second->get_market_data();
        }
        
        // Return empty market data
        MarketDataMessage md = {};
        md.header.msg_type = MessageType::MARKET_DATA;
        md.header.msg_length = sizeof(MarketDataMessage);
        md.header.timestamp = get_timestamp();
        std::strncpy(md.symbol, symbol.c_str(), sizeof(md.symbol) - 1);
        return md;
    }
    
    // Improved order processing with better error handling and connection monitoring
    void process_orders() {
        std::cout << "Order processing thread started" << std::endl;
        
        while (running_ && !shutdown_requested_) {
            try {
                bool connected = false;
                std::unique_ptr<aeron_wrapper::SubscriptionWrapper>* subscriber_ref = nullptr;
                
                {
                    std::lock_guard<std::mutex> lock(aeron_mutex_);
                    connected = aeron_connected_ && order_subscriber_;
                    if (connected) {
                        subscriber_ref = &order_subscriber_;
                    }
                }
                
                if (connected && *subscriber_ref) {
                    int fragments_read = (*subscriber_ref)->poll([this](const aeron_wrapper::FragmentData& fragment) {
                        try {
                            handle_incoming_message(fragment);
                        } catch (const std::exception& e) {
                            std::cerr << "Error handling message: " << e.what() << std::endl;
                        }
                    }, 10);
                    
                    // Add small delay if no fragments were read
                    if (fragments_read == 0) {
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                } else {
                    // Not connected, wait longer
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    
                    // Attempt to reconnect if needed
                    if (!connected && !shutdown_requested_) {
                        attempt_reconnection();
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "Error in order processing loop: " << e.what() << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                
                // On error, mark as disconnected and attempt reconnection
                aeron_connected_ = false;
            }
        }
        
        std::cout << "Order processing thread stopped" << std::endl;
    }
    
    // Publish market data periodically (Aeron mode)
    void publish_market_data() {
        std::cout << "Market data thread started" << std::endl;
        
        while (running_ && !shutdown_requested_) {
            try {
                bool connected = false;
                std::unique_ptr<aeron_wrapper::PublicationWrapper>* publisher_ref = nullptr;
                
                {
                    std::lock_guard<std::mutex> lock(aeron_mutex_);
                    connected = aeron_connected_ && market_data_publisher_;
                    if (connected) {
                        publisher_ref = &market_data_publisher_;
                    }
                }
                
                if (connected && *publisher_ref) {
                    std::vector<std::pair<std::string, std::unique_ptr<OrderBook>*>> books_snapshot;
                    
                    // Take snapshot of order books
                    {
                        std::lock_guard<std::mutex> lock(engine_mutex_);
                        for (auto& [symbol, book] : order_books_) {
                            books_snapshot.emplace_back(symbol, &book);
                        }
                    }
                    
                    // Publish market data for each symbol
                    for (const auto& [symbol, book_ptr] : books_snapshot) {
                        auto market_data = (*book_ptr)->get_market_data();
                        market_data.header.sequence_number = sequence_number_++;
                        
                        auto result = (*publisher_ref)->offer_with_retry(
                            reinterpret_cast<const std::uint8_t*>(&market_data),
                            sizeof(market_data));
                          
                        if (result != aeron_wrapper::PublicationResult::SUCCESS) {
                            std::cerr << "Failed to publish market data for " << symbol 
                                     << " (result: " << aeron_wrapper::PublicationWrapper::result_to_string(result) << ")" << std::endl;
                        }
                    }
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } catch (const std::exception& e) {
                std::cerr << "Error in market data publishing: " << e.what() << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                
                // On error, mark as disconnected
                aeron_connected_ = false;
            }
        }
        
        std::cout << "Market data thread stopped" << std::endl;
    }
    
    bool is_running() const { return running_; }
    bool is_aeron_connected() const { return aeron_connected_; }
    
    // Get all symbols
    std::vector<std::string> get_symbols() const {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        std::vector<std::string> symbols;
        for (const auto& [symbol, book] : order_books_) {
            symbols.push_back(symbol);
        }
        return symbols;
    }
    
    // Force reconnection attempt
    bool reconnect() {
        std::lock_guard<std::mutex> lock(aeron_mutex_);
        return initialize_aeron_connections();
    }
    
private:
    bool initialize_aeron_with_retry(const std::string& aeron_dir, int max_retries) {
        for (int attempt = 1; attempt <= max_retries; ++attempt) {
            std::cout << "Aeron initialization attempt " << attempt << "/" << max_retries << std::endl;
            
            try {
                std::lock_guard<std::mutex> lock(aeron_mutex_);
                
                // Clean up any existing resources
                order_subscriber_.reset();
                order_response_publisher_.reset();
                market_data_publisher_.reset();
                aeron_client_.reset();
                
                // Create new Aeron client
                aeron_client_ = std::make_unique<aeron_wrapper::AeronClient>(aeron_dir);
                
                // Give the client time to initialize
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                
                if (initialize_aeron_connections()) {
                    aeron_connected_ = true;
                    std::cout << "Aeron initialized successfully on attempt " << attempt << std::endl;
                    return true;
                }
                
            } catch (const std::exception& e) {
                std::cerr << "Aeron initialization attempt " << attempt << " failed: " << e.what() << std::endl;
            }
            
            if (attempt < max_retries) {
                std::cout << "Retrying in 1 second..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        
        return false;
    }
    
    bool initialize_aeron_connections() {
        try {
            if (!aeron_client_) {
                std::cerr << "Aeron client is null" << std::endl;
                return false;
            }
            
            std::cout << "Creating Aeron publications and subscriptions..." << std::endl;
            
            // Create publishers and subscribers
            order_response_publisher_ = aeron_client_->create_publication("aeron:ipc", 1001);
            if (!order_response_publisher_) {
                std::cerr << "Failed to create order response publisher" << std::endl;
                return false;
            }

            market_data_publisher_ = aeron_client_->create_publication("aeron:ipc", 1002);
            if (!market_data_publisher_) {
                std::cerr << "Failed to create market data publisher" << std::endl;
                return false;
            }
            
            order_subscriber_ = aeron_client_->create_subscription("aeron:ipc", 1000);
            if (!order_subscriber_) {
                std::cerr << "Failed to create order subscriber" << std::endl;
                return false;
            }

            std::cout << "Waiting for Aeron connections..." << std::endl;
            
            // Wait for connections with timeout
            return true; //wait_for_connections();
            
        } catch (const std::exception& e) {
            std::cerr << "Exception in initialize_aeron_connections: " << e.what() << std::endl;
            return false;
        }
    }
    
    bool wait_for_connections() {
        auto start_time = std::chrono::steady_clock::now();
        auto timeout = std::chrono::milliseconds(CONNECTION_TIMEOUT_MS);
        
        std::cout << "Waiting for connections (timeout: " << CONNECTION_TIMEOUT_MS << "ms)..." << std::endl;
        
        while (std::chrono::steady_clock::now() - start_time < timeout) {
            try {
                bool order_response_connected = order_response_publisher_ && order_response_publisher_->is_connected();
                bool market_data_connected = market_data_publisher_ && market_data_publisher_->is_connected();
                bool order_sub_connected = order_subscriber_ && order_subscriber_->is_connected();
                
                std::cout << "Connection status - Order Response: " << (order_response_connected ? "OK" : "NO") 
                         << ", Market Data: " << (market_data_connected ? "OK" : "NO")
                         << ", Order Sub: " << (order_sub_connected ? "OK" : "NO") << std::endl;
                
                if (order_response_connected && market_data_connected && order_sub_connected) {
                    std::cout << "All Aeron connections established successfully!" << std::endl;
                    return true;
                }
            } catch (const std::exception& e) {
                std::cerr << "Connection check error: " << e.what() << std::endl;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(CONNECTION_CHECK_INTERVAL_MS));
        }
        
        std::cout << "Connection timeout reached" << std::endl;
        return false;
    }
    
    void attempt_reconnection() {
        static auto last_reconnect_attempt = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        
        // Only attempt reconnection every 5 seconds
        if (now - last_reconnect_attempt < std::chrono::seconds(3)) {
            return;
        }
        
        last_reconnect_attempt = now;
        std::cout << "Attempting to reconnect Aeron..." << std::endl;
        
        {
            std::lock_guard<std::mutex> lock(aeron_mutex_);
            aeron_connected_ = initialize_aeron_connections();
            if (aeron_connected_) {
                std::cout << "Reconnection successful!" << std::endl;
            } else {
                std::cout << "Reconnection failed" << std::endl;
            }
        }
    }

    void handle_incoming_message(const aeron_wrapper::FragmentData& fragment) {
        if (fragment.length < sizeof(MessageHeader)) {
            return;
        }
        
        const auto* header = reinterpret_cast<const MessageHeader*>(fragment.buffer);
        
        switch (header->msg_type) {
            case MessageType::NEW_ORDER:
                handle_new_order(fragment);
                break;
            case MessageType::CANCEL_ORDER:
                handle_cancel_order(fragment);
                break;
            default:
                std::cerr << "Unknown message type: " << static_cast<int>(header->msg_type) << std::endl;
                break;
        }
    }
    
    void handle_new_order(const aeron_wrapper::FragmentData& fragment) {
        if (fragment.length < sizeof(NewOrderMessage)) {
            std::cerr << "Invalid new order message size" << std::endl;
            return;
        }
        
        const auto* msg = reinterpret_cast<const NewOrderMessage*>(fragment.buffer);
        
        std::string symbol(msg->symbol);
        symbol = symbol.substr(0, symbol.find('\0')); // Remove null padding
        
        if (symbol.empty()) {
            std::cerr << "Empty symbol in new order message" << std::endl;
            return;
        }
        
        // Get or create order book
        std::unique_ptr<OrderBook>* book_ptr = nullptr;
        {
            std::lock_guard<std::mutex> lock(engine_mutex_);
            auto it = order_books_.find(symbol);
            if (it == order_books_.end()) {
                order_books_[symbol] = std::make_unique<OrderBook>(symbol);
                book_ptr = &order_books_[symbol];
            } else {
                book_ptr = &it->second;
            }
        }
        
        // Create order
        std::uint64_t order_id = (msg->order_id == 0) ? next_order_id_++ : msg->order_id;
        std::string client_order_id(msg->client_order_id);
        client_order_id = client_order_id.substr(0, client_order_id.find('\0'));
        
        auto order = std::make_shared<Order>(
            order_id, symbol, msg->side, msg->type,
            msg->quantity, msg->price, msg->client_id, client_order_id);
        
        // Send acknowledgment
        send_order_ack(order);
        
        // Process order
        auto trades = (*book_ptr)->add_order(order);
        
        // Send fill reports
        for (const auto& trade : trades) {
            send_fill_report(trade);
        }
    }
    
    void handle_cancel_order(const aeron_wrapper::FragmentData& fragment) {
        if (fragment.length < sizeof(CancelOrderMessage)) {
            std::cerr << "Invalid cancel order message size" << std::endl;
            return;
        }
        
        const auto* msg = reinterpret_cast<const CancelOrderMessage*>(fragment.buffer);
        
        // Find the order across all books
        std::shared_ptr<Order> order = nullptr;
        {
            std::lock_guard<std::mutex> lock(engine_mutex_);
            for (const auto& [symbol, book] : order_books_) {
                order = book->get_order(msg->order_id);
                if (order) {
                    bool cancelled = book->cancel_order(msg->order_id);
                    if (cancelled) {
                        send_cancel_ack(order);
                    }
                    break;
                }
            }
        }
        
        if (!order) {
            std::cerr << "Order not found for cancellation: " << msg->order_id << std::endl;
        }
    }
    
    void send_order_ack(std::shared_ptr<Order> order) {
        try {
            std::lock_guard<std::mutex> lock(aeron_mutex_);
            if (!aeron_connected_ || !order_response_publisher_) {
                return;
            }
            std::cout << "Sending order ack for order ID: " << order->order_id << std::endl;
            OrderAckMessage ack = {};
            ack.header.msg_type = MessageType::ORDER_ACK;
            ack.header.msg_length = sizeof(OrderAckMessage);
            ack.header.timestamp = get_timestamp();
            ack.header.sequence_number = sequence_number_++;
            
            ack.order_id = order->order_id;
            ack.status = order->status;
            std::strncpy(ack.symbol, order->symbol.c_str(), sizeof(ack.symbol) - 1);
            ack.client_id = order->client_id;
            std::strncpy(ack.client_order_id, order->client_order_id.c_str(), sizeof(ack.client_order_id) - 1);
            
            auto result = order_response_publisher_->offer_with_retry(
                reinterpret_cast<const std::uint8_t*>(&ack),
                sizeof(ack));
            
            if (result != aeron_wrapper::PublicationResult::SUCCESS) {
                std::cerr << "Failed to send order ack (result: " << aeron_wrapper::PublicationWrapper::result_to_string(result) << ")" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error sending order ack: " << e.what() << std::endl;
        }
    }
    
    void send_fill_report(const OrderBook::Trade& trade) {
        // Send fill report for aggressive order
        send_fill_report_for_order(trade.aggressive_order, trade, trade.passive_order->order_id);
        
        // Send fill report for passive order
        send_fill_report_for_order(trade.passive_order, trade, trade.aggressive_order->order_id);
    }
    
    void send_fill_report_for_order(std::shared_ptr<Order> order, const OrderBook::Trade& trade, 
                                   std::uint64_t counterparty_order_id) {
        try {
            std::lock_guard<std::mutex> lock(aeron_mutex_);
            if (!aeron_connected_ || !order_response_publisher_) {
                return;
            }
            
            FillReportMessage fill = {};
            fill.header.msg_type = MessageType::FILL_REPORT;
            fill.header.msg_length = sizeof(FillReportMessage);
            fill.header.timestamp = get_timestamp();
            fill.header.sequence_number = sequence_number_++;
            
            fill.order_id = order->order_id;
            fill.fill_id = next_fill_id_++;
            std::strncpy(fill.symbol, order->symbol.c_str(), sizeof(fill.symbol) - 1);
            fill.side = order->side;
            fill.fill_quantity = trade.quantity;
            fill.fill_price = trade.price;
            fill.leaves_quantity = order->remaining_quantity;
            fill.client_id = order->client_id;
            std::strncpy(fill.client_order_id, order->client_order_id.c_str(), sizeof(fill.client_order_id) - 1);
            fill.counterparty_order_id = counterparty_order_id;
            
            auto result = order_response_publisher_->offer_with_retry(
                reinterpret_cast<const std::uint8_t*>(&fill),
                sizeof(fill));
            
            if (result != aeron_wrapper::PublicationResult::SUCCESS) {
                std::cerr << "Failed to send fill report (result: " << aeron_wrapper::PublicationWrapper::result_to_string(result) << ")" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error sending fill report: " << e.what() << std::endl;
        }
    }
    
    void send_cancel_ack(std::shared_ptr<Order> order) {
        try {
            std::lock_guard<std::mutex> lock(aeron_mutex_);
            if (!aeron_connected_ || !order_response_publisher_) {
                return;
            }
            
            OrderAckMessage ack = {};
            ack.header.msg_type = MessageType::CANCEL_ACK;
            ack.header.msg_length = sizeof(OrderAckMessage);
            ack.header.timestamp = get_timestamp();
            ack.header.sequence_number = sequence_number_++;
            
            ack.order_id = order->order_id;
            ack.status = OrderStatus::CANCELLED;
            std::strncpy(ack.symbol, order->symbol.c_str(), sizeof(ack.symbol) - 1);
            ack.client_id = order->client_id;
            std::strncpy(ack.client_order_id, order->client_order_id.c_str(), sizeof(ack.client_order_id) - 1);
            
            auto result = order_response_publisher_->offer_with_retry(
                reinterpret_cast<const std::uint8_t*>(&ack),
                sizeof(ack));
            
            if (result != aeron_wrapper::PublicationResult::SUCCESS) {
                std::cerr << "Failed to send cancel ack (result: " << static_cast<int>(result) << ")" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error sending cancel ack: " << e.what() << std::endl;
        }
    }
    
    static std::uint64_t get_timestamp() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }
};

// Trading client for testing
class TradingClient {
private:
    std::unique_ptr<aeron_wrapper::AeronClient> aeron_client_;
    std::unique_ptr<aeron_wrapper::PublicationWrapper> order_publisher_;
    std::unique_ptr<aeron_wrapper::SubscriptionWrapper> response_subscriber_;
    std::unique_ptr<aeron_wrapper::SubscriptionWrapper> market_data_subscriber_;
    
    std::atomic<std::uint32_t> client_id_;
    std::atomic<std::uint32_t> next_client_order_id_{1};
    
public:
    explicit TradingClient(std::uint32_t client_id, const std::string& aeron_dir = "")
        : client_id_(client_id) {
        
        aeron_client_ = std::make_unique<aeron_wrapper::AeronClient>(aeron_dir);
        
        order_publisher_ = aeron_client_->create_publication("aeron:ipc", 1000);
        response_subscriber_ = aeron_client_->create_subscription("aeron:ipc", 1001);
        market_data_subscriber_ = aeron_client_->create_subscription("aeron:ipc", 1002);
        
        // Wait for connections
        while (!order_publisher_->is_connected() || 
               !response_subscriber_->is_connected() ||
               !market_data_subscriber_->is_connected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    // Send new order
    bool send_order(const std::string& symbol, OrderSide side, OrderType type,
                   std::uint64_t quantity, std::uint64_t price = 0) {
        
        NewOrderMessage order = {};
        order.header.msg_type = MessageType::NEW_ORDER;
        order.header.msg_length = sizeof(NewOrderMessage);
        order.header.timestamp = get_timestamp();
        order.header.sequence_number = 0; // Will be set by engine
        
        order.order_id = 0; // Will be assigned by engine
        std::strncpy(order.symbol, symbol.c_str(), sizeof(order.symbol) - 1);
        order.side = side;
        order.type = type;
        order.quantity = quantity;
        order.price = price;
        order.client_id = client_id_;
        
        std::string client_order_id = "CLIENT_" + std::to_string(client_id_) + "_" + std::to_string(next_client_order_id_++);
        std::strncpy(order.client_order_id, client_order_id.c_str(), sizeof(order.client_order_id) - 1);
        
        auto result = order_publisher_->offer_with_retry(
            reinterpret_cast<const std::uint8_t*>(&order),
            sizeof(order));
        
        return result == aeron_wrapper::PublicationResult::SUCCESS;
    }
    
    // Send cancel order
    bool cancel_order(std::uint64_t order_id, const std::string& orig_client_order_id) {
        CancelOrderMessage cancel = {};
        cancel.header.msg_type = MessageType::CANCEL_ORDER;
        cancel.header.msg_length = sizeof(CancelOrderMessage);
        cancel.header.timestamp = get_timestamp();
        cancel.header.sequence_number = 0;
        
        cancel.order_id = order_id;
        cancel.client_id = client_id_;
        std::strncpy(cancel.orig_client_order_id, orig_client_order_id.c_str(), sizeof(cancel.orig_client_order_id) - 1);
        
        auto result = order_publisher_->offer_with_retry(
            reinterpret_cast<const std::uint8_t*>(&cancel),
            sizeof(cancel));
        
        return result == aeron_wrapper::PublicationResult::SUCCESS;
    }
    
    // Poll for responses
    void poll_responses(std::function<void(const MessageHeader*, const std::uint8_t*)> handler) {
        response_subscriber_->poll([&handler](const aeron_wrapper::FragmentData& fragment) {
            if (fragment.length >= sizeof(MessageHeader)) {
                const auto* header = reinterpret_cast<const MessageHeader*>(fragment.buffer);
                handler(header, fragment.buffer);
            }
        });
    }
    
    // Poll for market data
    void poll_market_data(std::function<void(const MarketDataMessage&)> handler) {
        market_data_subscriber_->poll([&handler](const aeron_wrapper::FragmentData& fragment) {
            if (fragment.length >= sizeof(MarketDataMessage)) {
                const auto* md = reinterpret_cast<const MarketDataMessage*>(fragment.buffer);
                handler(*md);
            }
        });
    }
    
    std::uint32_t get_client_id() const { return client_id_; }
    
private:
    static std::uint64_t get_timestamp() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }
};

} // namespace matching_engine

// Utility functions for price formatting
namespace matching_engine {
namespace utils {

// Convert price from ticks to decimal string (assuming 2 decimal places)
std::string format_price(std::uint64_t price_ticks, int decimal_places = 2) {
    double price = static_cast<double>(price_ticks) / std::pow(10, decimal_places);
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(decimal_places) << price;
    return oss.str();
}

// Convert decimal price to ticks
std::uint64_t price_to_ticks(double price, int decimal_places = 2) {
    return static_cast<std::uint64_t>(price * std::pow(10, decimal_places) + 0.5);
}

// Order side to string
std::string side_to_string(OrderSide side) {
    return (side == OrderSide::BUY) ? "BUY" : "SELL";
}

// Order type to string
std::string type_to_string(OrderType type) {
    switch (type) {
        case OrderType::MARKET: return "MARKET";
        case OrderType::LIMIT: return "LIMIT";
        case OrderType::STOP: return "STOP";
        case OrderType::STOP_LIMIT: return "STOP_LIMIT";
        default: return "UNKNOWN";
    }
}

// Order status to string
std::string status_to_string(OrderStatus status) {
    switch (status) {
        case OrderStatus::NEW: return "NEW";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::FILLED: return "FILLED";
        case OrderStatus::CANCELLED: return "CANCELLED";
        case OrderStatus::REJECTED: return "REJECTED";
        case OrderStatus::EXPIRED: return "EXPIRED";
        default: return "UNKNOWN";
    }
}

// Print order book state (for debugging)
void print_order_summary(const Order& order) {
    std::cout << "Order ID: " << order.order_id
              << ", Symbol: " << order.symbol
              << ", Side: " << side_to_string(order.side)
              << ", Type: " << type_to_string(order.type)
              << ", Original Qty: " << order.original_quantity
              << ", Remaining Qty: " << order.remaining_quantity
              << ", Price: " << format_price(order.price)
              << ", Status: " << status_to_string(order.status)
              << ", Client: " << order.client_id
              << ", Client Order ID: " << order.client_order_id << std::endl;
}

// Print market data
void print_market_data(const MarketDataMessage& md) {
    std::string symbol(md.symbol);
    symbol = symbol.substr(0, symbol.find('\0'));
    
    std::cout << "=== Market Data for " << symbol << " ===" << std::endl;
    std::cout << "Bid: " << format_price(md.bid_price) << " x " << md.bid_quantity << std::endl;
    std::cout << "Ask: " << format_price(md.ask_price) << " x " << md.ask_quantity << std::endl;
    std::cout << "Last: " << format_price(md.last_price) << " x " << md.last_quantity << std::endl;
    std::cout << "Volume: " << md.total_volume << ", Trades: " << md.trade_count << std::endl;
    std::cout << "=================================" << std::endl;
}

} // namespace utils
} // namespace matching_engine
