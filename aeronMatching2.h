
#include <iostream>
#include <map>
#include <queue>
#include <memory>
#include <string>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <thread>
#include <chrono>
#include <random>

#include "aeronWrapper.h" // Your provided header

static const std::string AeronDir = "";
static const std::string AeronIPC = "aeron:ipc";

namespace matching_engine {

// Order types
enum class Side : uint8_t {
    BUY = 0,
    SELL = 1
};

enum class OrderType : uint8_t {
    LIMIT = 0,
    MARKET = 1,
    STOP = 2,
    STOP_LIMIT = 3
};

enum class OrderStatus : uint8_t {
    PENDING = 0,
    ACTIVE = 1,
    FILLED = 2,
    CANCELLED = 3,
    TRIGGERED = 4
};

// Enhanced order structure
struct Order {
    uint64_t order_id;
    uint64_t client_id;
    std::string symbol;
    Side side;
    OrderType type;
    double price;           // Limit price for LIMIT/STOP_LIMIT orders
    double stop_price;      // Trigger price for STOP/STOP_LIMIT orders
    uint64_t quantity;
    uint64_t remaining_quantity;
    uint64_t timestamp;
    OrderStatus status;
    
    Order(uint64_t id, uint64_t client, const std::string& sym, Side s, OrderType t, 
          double p, uint64_t q, double stop_p = 0.0, uint64_t ts = 0, OrderStatus st = OrderStatus::PENDING)
        : order_id(id), client_id(client), symbol(sym), side(s), type(t), 
          price(p), stop_price(stop_p), quantity(q), remaining_quantity(q), 
          timestamp(ts == 0 ? std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::high_resolution_clock::now().time_since_epoch()).count() : ts),
          status(st) {}
};

// Trade execution result
struct Trade {
    uint64_t trade_id;
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    uint64_t buy_client_id;
    uint64_t sell_client_id;
    std::string symbol;
    double price;
    uint64_t quantity;
    uint64_t timestamp;
    
    Trade(uint64_t id, const Order& buy_order, const Order& sell_order, 
          double exec_price, uint64_t exec_qty)
        : trade_id(id), buy_order_id(buy_order.order_id), sell_order_id(sell_order.order_id),
          buy_client_id(buy_order.client_id), sell_client_id(sell_order.client_id),
          symbol(buy_order.symbol), price(exec_price), quantity(exec_qty),
          timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::high_resolution_clock::now().time_since_epoch()).count()) {}
};

// Message serialization helpers
std::string serialize_order(const Order& order) {
    std::ostringstream oss;
    oss << "ORDER|" << order.order_id << "|" << order.client_id << "|" 
        << order.symbol << "|" << static_cast<int>(order.side) << "|"
        << static_cast<int>(order.type) << "|" << std::fixed << std::setprecision(2) 
        << order.price << "|" << order.stop_price << "|" << order.quantity << "|" 
        << static_cast<int>(order.status) << "|" << order.timestamp;
    return oss.str();
}

std::string serialize_trade(const Trade& trade) {
    std::ostringstream oss;
    oss << "TRADE|" << trade.trade_id << "|" << trade.buy_order_id << "|" 
        << trade.sell_order_id << "|" << trade.buy_client_id << "|" 
        << trade.sell_client_id << "|" << trade.symbol << "|" 
        << std::fixed << std::setprecision(2) << trade.price << "|" 
        << trade.quantity << "|" << trade.timestamp;
    return oss.str();
}

std::string serialize_order_ack(uint64_t order_id, const std::string& status) {
    std::ostringstream oss;
    oss << "ACK|" << order_id << "|" << status;
    return oss.str();
}

std::string serialize_stop_trigger(uint64_t order_id, double trigger_price) {
    std::ostringstream oss;
    oss << "STOP_TRIGGERED|" << order_id << "|" << std::fixed << std::setprecision(2) << trigger_price;
    return oss.str();
}

// Order book for a single symbol
class OrderBook {
private:
    std::string symbol_;
    
    // Buy orders (highest price first)
    std::map<double, std::queue<std::shared_ptr<Order>>, std::greater<double>> buy_orders_;
    
    // Sell orders (lowest price first)
    std::map<double, std::queue<std::shared_ptr<Order>>> sell_orders_;
    
    // Stop orders waiting to be triggered
    std::vector<std::shared_ptr<Order>> stop_orders_;
    
    // Last traded price for stop order triggering
    double last_traded_price_;
    bool has_last_price_;
    
    std::mutex book_mutex_;
    
public:
    explicit OrderBook(const std::string& symbol) 
        : symbol_(symbol), last_traded_price_(0.0), has_last_price_(false) {}
    
    std::vector<Trade> add_order(std::shared_ptr<Order> order) {
        std::lock_guard<std::mutex> lock(book_mutex_);
        std::vector<Trade> trades;
        
        // Handle stop orders
        if (order->type == OrderType::STOP || order->type == OrderType::STOP_LIMIT) {
            if (should_trigger_stop_order(order)) {
                // Convert stop order to market/limit order
                convert_stop_order(order);
                // Process the converted order
                trades = process_regular_order(order);
            } else {
                // Add to stop orders list
                stop_orders_.push_back(order);
                order->status = OrderStatus::PENDING;
            }
        } else {
            // Regular market/limit order
            trades = process_regular_order(order);
        }
        
        // Update last traded price if trades occurred
        if (!trades.empty()) {
            last_traded_price_ = trades.back().price;
            has_last_price_ = true;
            
            // Check if any stop orders should be triggered
            auto triggered_trades = check_and_trigger_stop_orders();
            trades.insert(trades.end(), triggered_trades.begin(), triggered_trades.end());
        }
        
        return trades;
    }
    
    bool cancel_order(uint64_t order_id) {
        std::lock_guard<std::mutex> lock(book_mutex_);
        
        // First check stop orders
        for (auto it = stop_orders_.begin(); it != stop_orders_.end(); ++it) {
            if ((*it)->order_id == order_id) {
                (*it)->status = OrderStatus::CANCELLED;
                stop_orders_.erase(it);
                return true;
            }
        }
        
        // Check buy orders
        if (cancel_from_order_map(buy_orders_, order_id)) {
            return true;
        }
        
        // Check sell orders
        if (cancel_from_order_map(sell_orders_, order_id)) {
            return true;
        }
        
        return false; // Order not found
    }
    
    void print_book() {
        std::lock_guard<std::mutex> lock(book_mutex_);
        std::cout << "\n=== Order Book for " << symbol_ << " ===\n";
        
        if (has_last_price_) {
            std::cout << "Last Price: " << std::fixed << std::setprecision(2) 
                     << last_traded_price_ << "\n\n";
        }
        
        // Print sell orders (asks) from highest to lowest
        std::cout << "ASKS:\n";
        for (auto it = sell_orders_.rbegin(); it != sell_orders_.rend(); ++it) {
            if (!it->second.empty()) {
                uint64_t total_qty = 0;
                auto temp_queue = it->second;
                while (!temp_queue.empty()) {
                    total_qty += temp_queue.front()->remaining_quantity;
                    temp_queue.pop();
                }
                std::cout << "  " << std::fixed << std::setprecision(2) 
                         << it->first << " x " << total_qty << "\n";
            }
        }
        
        std::cout << "------\n";
        
        // Print buy orders (bids) from highest to lowest
        std::cout << "BIDS:\n";
        for (const auto& [price, orders] : buy_orders_) {
            if (!orders.empty()) {
                uint64_t total_qty = 0;
                auto temp_queue = orders;
                while (!temp_queue.empty()) {
                    total_qty += temp_queue.front()->remaining_quantity;
                    temp_queue.pop();
                }
                std::cout << "  " << std::fixed << std::setprecision(2) 
                         << price << " x " << total_qty << "\n";
            }
        }
        
        // Print pending stop orders
        if (!stop_orders_.empty()) {
            std::cout << "\nPENDING STOP ORDERS:\n";
            for (const auto& order : stop_orders_) {
                std::cout << "  " << (order->side == Side::BUY ? "BUY" : "SELL") 
                         << " " << (order->type == OrderType::STOP ? "STOP" : "STOP_LIMIT")
                         << " @ " << std::fixed << std::setprecision(2) << order->stop_price;
                if (order->type == OrderType::STOP_LIMIT) {
                    std::cout << " -> " << order->price;
                }
                std::cout << " x " << order->remaining_quantity 
                         << " (ID: " << order->order_id << ")\n";
            }
        }
        
        std::cout << "========================\n\n";
    }
    
    // Get current best bid/ask for external price monitoring
    std::pair<double, double> get_best_bid_ask() {
        std::lock_guard<std::mutex> lock(book_mutex_);
        double best_bid = 0.0;
        double best_ask = 0.0;
        
        if (!buy_orders_.empty()) {
            best_bid = buy_orders_.begin()->first;
        }
        
        if (!sell_orders_.empty()) {
            best_ask = sell_orders_.begin()->first;
        }
        
        return {best_bid, best_ask};
    }
    
private:
    template<typename OrderMap>
    bool cancel_from_order_map(OrderMap& order_map, uint64_t order_id) {
        for (auto& [price, order_queue] : order_map) {
            if (order_queue.empty()) continue;
            
            // We need to search through the queue to find the order
            std::queue<std::shared_ptr<Order>> temp_queue;
            bool found = false;
            
            while (!order_queue.empty()) {
                auto order = order_queue.front();
                order_queue.pop();
                
                if (order->order_id == order_id && !found) {
                    // Found the order to cancel
                    order->status = OrderStatus::CANCELLED;
                    found = true;
                    // Don't add it back to temp_queue
                } else {
                    // Keep this order
                    temp_queue.push(order);
                }
            }
            
            // Restore the queue without the cancelled order
            order_queue = std::move(temp_queue);
            
            if (found) {
                // Clean up empty price level
                if (order_queue.empty()) {
                    order_map.erase(price);
                }
                return true;
            }
        }
        return false;
    }
    
    bool should_trigger_stop_order(std::shared_ptr<Order> order) {
        if (!has_last_price_) {
            return false; // No reference price yet
        }
        
        if (order->side == Side::BUY) {
            // Buy stop: trigger when price rises to or above stop price
            return last_traded_price_ >= order->stop_price;
        } else {
            // Sell stop: trigger when price falls to or below stop price
            return last_traded_price_ <= order->stop_price;
        }
    }
    
    void convert_stop_order(std::shared_ptr<Order> order) {
        if (order->type == OrderType::STOP) {
            // Convert to market order
            order->type = OrderType::MARKET;
            order->price = 0.0; // Market orders don't need price
        } else if (order->type == OrderType::STOP_LIMIT) {
            // Convert to limit order
            order->type = OrderType::LIMIT;
            // Price is already set as the limit price
        }
        order->status = OrderStatus::TRIGGERED;
    }
    
    std::vector<Trade> process_regular_order(std::shared_ptr<Order> order) {
        std::vector<Trade> trades;
        
        if (order->side == Side::BUY) {
            trades = match_buy_order(order);
            if (order->remaining_quantity > 0 && order->type == OrderType::LIMIT) {
                buy_orders_[order->price].push(order);
                order->status = OrderStatus::ACTIVE;
            }
        } else {
            trades = match_sell_order(order);
            if (order->remaining_quantity > 0 && order->type == OrderType::LIMIT) {
                sell_orders_[order->price].push(order);
                order->status = OrderStatus::ACTIVE;
            }
        }
        
        // Mark as filled if completely executed
        if (order->remaining_quantity == 0) {
            order->status = OrderStatus::FILLED;
        }
        
        return trades;
    }
    
    std::vector<Trade> check_and_trigger_stop_orders() {
        std::vector<Trade> all_trades;
        std::vector<std::shared_ptr<Order>> triggered_orders;
        
        // Find orders to trigger
        for (auto it = stop_orders_.begin(); it != stop_orders_.end();) {
            if (should_trigger_stop_order(*it)) {
                triggered_orders.push_back(*it);
                it = stop_orders_.erase(it);
            } else {
                ++it;
            }
        }
        
        // Process triggered orders
        for (auto& order : triggered_orders) {
            convert_stop_order(order);
            auto trades = process_regular_order(order);
            all_trades.insert(all_trades.end(), trades.begin(), trades.end());
            
            // Update last price if new trades occurred
            if (!trades.empty()) {
                last_traded_price_ = trades.back().price;
            }
        }
        
        return all_trades;
    }
    
    std::vector<Trade> match_buy_order(std::shared_ptr<Order> buy_order) {
        std::vector<Trade> trades;
        static uint64_t trade_id_counter = 1;
        
        // Match against sell orders (lowest price first)
        for (auto& [price, sell_queue] : sell_orders_) {
            if (buy_order->remaining_quantity == 0) break;
            
            // For market orders, match at any price
            // For limit orders, only match if price is acceptable
            if (buy_order->type == OrderType::LIMIT && price > buy_order->price) {
                break; // No more matches possible
            }
            
            while (!sell_queue.empty() && buy_order->remaining_quantity > 0) {
                auto sell_order = sell_queue.front();
                
                uint64_t trade_qty = std::min(buy_order->remaining_quantity, 
                                            sell_order->remaining_quantity);
                double trade_price = sell_order->price; // Price improvement for buyer
                
                // Create trade
                trades.emplace_back(trade_id_counter++, *buy_order, *sell_order, 
                                  trade_price, trade_qty);
                
                // Update quantities
                buy_order->remaining_quantity -= trade_qty;
                sell_order->remaining_quantity -= trade_qty;
                
                // Remove fully filled sell order
                if (sell_order->remaining_quantity == 0) {
                    sell_order->status = OrderStatus::FILLED;
                    sell_queue.pop();
                }
            }
        }
        
        // Clean up empty price levels
        for (auto it = sell_orders_.begin(); it != sell_orders_.end();) {
            if (it->second.empty()) {
                it = sell_orders_.erase(it);
            } else {
                ++it;
            }
        }
        
        return trades;
    }
    
    std::vector<Trade> match_sell_order(std::shared_ptr<Order> sell_order) {
        std::vector<Trade> trades;
        static uint64_t trade_id_counter = 1;
        
        // Match against buy orders (highest price first)
        for (auto& [price, buy_queue] : buy_orders_) {
            if (sell_order->remaining_quantity == 0) break;
            
            // For market orders, match at any price
            // For limit orders, only match if price is acceptable
            if (sell_order->type == OrderType::LIMIT && price < sell_order->price) {
                break; // No more matches possible
            }
            
            while (!buy_queue.empty() && sell_order->remaining_quantity > 0) {
                auto buy_order = buy_queue.front();
                
                uint64_t trade_qty = std::min(sell_order->remaining_quantity, 
                                            buy_order->remaining_quantity);
                double trade_price = buy_order->price; // Price improvement for seller
                
                // Create trade
                trades.emplace_back(trade_id_counter++, *buy_order, *sell_order, 
                                  trade_price, trade_qty);
                
                // Update quantities
                sell_order->remaining_quantity -= trade_qty;
                buy_order->remaining_quantity -= trade_qty;
                
                // Remove fully filled buy order
                if (buy_order->remaining_quantity == 0) {
                    buy_order->status = OrderStatus::FILLED;
                    buy_queue.pop();
                }
            }
        }
        
        // Clean up empty price levels
        for (auto it = buy_orders_.begin(); it != buy_orders_.end();) {
            if (it->second.empty()) {
                it = buy_orders_.erase(it);
            } else {
                ++it;
            }
        }
        
        return trades;
    }
};

// Main matching engine
class MatchingEngine {
private:
    std::map<std::string, std::unique_ptr<OrderBook>> order_books_;
    std::mutex engine_mutex_;
    
    // Aeron components
    std::unique_ptr<aeron_wrapper::AeronClient> aeron_client_;
    std::unique_ptr<aeron_wrapper::SubscriptionWrapper> order_subscription_;
    std::unique_ptr<aeron_wrapper::PublicationWrapper> trade_publication_;
    std::unique_ptr<aeron_wrapper::PublicationWrapper> ack_publication_;
    
    std::atomic<bool> running_{false};
    std::unique_ptr<aeron_wrapper::SubscriptionWrapper::BackgroundPoller> background_poller_;
    
public:
    MatchingEngine(const std::string& aeron_dir = AeronDir) {
        // Initialize Aeron client
        aeron_client_ = std::make_unique<aeron_wrapper::AeronClient>(aeron_dir);
        
        // Create subscriptions and publications
        order_subscription_ = aeron_client_->create_subscription(AeronIPC, 1001); // Orders in
        trade_publication_ = aeron_client_->create_publication(AeronIPC, 1002);   // Trades out
        ack_publication_ = aeron_client_->create_publication(AeronIPC, 1003);     // Order acks out
        
        // // Wait for connections
        // std::cout << "Waiting for Aeron connections...\n";
        // while (!order_subscription_->is_connected() || 
        //        !trade_publication_->is_connected() ||
        //        !ack_publication_->is_connected()) {
        //     std::this_thread::sleep_for(std::chrono::milliseconds(10));
        // }
        std::cout << "All Aeron connections established!\n";
        
        running_ = true;
    }
    
    ~MatchingEngine() {
        stop();
    }
    
    void start() {
        if (!running_) return;
        
        std::cout << "Starting matching engine...\n";
        
        // Start background order processing
        background_poller_ = order_subscription_->start_background_polling(
            [this](const aeron_wrapper::FragmentData& fragment) {
                process_incoming_message(fragment);
            });
        
        std::cout << "Matching engine started!\n";
    }
    
    void stop() {
        if (running_) {
            running_ = false;
            if (background_poller_) {
                background_poller_->stop();
            }
            std::cout << "Matching engine stopped.\n";
        }
    }
    
    void print_all_books() {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        for (const auto& [symbol, book] : order_books_) {
            book->print_book();
        }
    }
    
private:
    void process_incoming_message(const aeron_wrapper::FragmentData& fragment) {
        try {
            std::string message = fragment.as_string();
            if (message.substr(0, 6) == "ORDER|") {
                process_order(message);
            }
        } catch (const std::exception& e) {
            std::cerr << "Error processing message: " << e.what() << std::endl;
        }
    }
    
    void process_order(const std::string& order_message) {
        // Parse order: ORDER|order_id|client_id|symbol|side|type|price|quantity|timestamp
        std::istringstream iss(order_message);
        std::string token;
        std::vector<std::string> tokens;
        
        while (std::getline(iss, token, '|')) {
            tokens.push_back(token);
        }
        
        if (tokens.size() != 11) {
            std::cerr << "Invalid order format: " << order_message << std::endl;
            return;
        }
        
        try {
            uint64_t order_id = std::stoull(tokens[1]);
            uint64_t client_id = std::stoull(tokens[2]);
            std::string symbol = tokens[3];
            Side side = static_cast<Side>(std::stoi(tokens[4]));
            OrderType type = static_cast<OrderType>(std::stoi(tokens[5]));
            double price = std::stod(tokens[6]);
            double stop_price = std::stod(tokens[7]);
            uint64_t quantity = std::stoull(tokens[8]);
            OrderStatus status = static_cast<OrderStatus>(std::stoi(tokens[9]));
            uint64_t timestamp = std::stoull(tokens[10]);
            
            auto order = std::make_shared<Order>(order_id, client_id, symbol, side, type, price, quantity, stop_price, timestamp, status);
            
            // Send acknowledgment
            std::string ack = serialize_order_ack(order_id, "ACCEPTED");
            ack_publication_->offer_with_retry(ack);
            
            // Process order through order book
            {
                std::lock_guard<std::mutex> lock(engine_mutex_);
                if (order_books_.find(symbol) == order_books_.end()) {
                    order_books_[symbol] = std::make_unique<OrderBook>(symbol);
                }
                
                auto trades = order_books_[symbol]->add_order(order);
                
                // Publish trades
                for (const auto& trade : trades) {
                    std::string trade_msg = serialize_trade(trade);
                    trade_publication_->offer_with_retry(trade_msg);
                    
                    std::cout << "TRADE: " << trade.symbol << " " << trade.quantity 
                             << " @ " << std::fixed << std::setprecision(2) << trade.price 
                             << " (Buy: " << trade.buy_client_id << ", Sell: " << trade.sell_client_id << ")\n";
                }
            }
            
        } catch (const std::exception& e) {
            std::cerr << "Error parsing order: " << e.what() << std::endl;
        }
    }
};

} // namespace matching_engine

// Example client for testing
class TestClient {
private:
    std::unique_ptr<aeron_wrapper::AeronClient> aeron_client_;
    std::unique_ptr<aeron_wrapper::PublicationWrapper> order_publication_;
    std::unique_ptr<aeron_wrapper::SubscriptionWrapper> trade_subscription_;
    std::unique_ptr<aeron_wrapper::SubscriptionWrapper> ack_subscription_;
    
    uint64_t client_id_;
    std::atomic<uint64_t> order_id_counter_{1};
    
public:
    TestClient(uint64_t client_id, const std::string& aeron_dir = AeronDir) 
        : client_id_(client_id) {
        
        aeron_client_ = std::make_unique<aeron_wrapper::AeronClient>(aeron_dir);
        
        order_publication_ = aeron_client_->create_publication(AeronIPC, 1001);   // Orders out
        trade_subscription_ = aeron_client_->create_subscription(AeronIPC, 1002); // Trades in
        ack_subscription_ = aeron_client_->create_subscription(AeronIPC, 1003);   // Acks in
        
        // Wait for connections
        while (!order_publication_->is_connected() || 
               !trade_subscription_->is_connected() ||
               !ack_subscription_->is_connected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        std::cout << "Test client " << client_id_ << " connected!\n";
    }
    
    void send_limit_order(const std::string& symbol, matching_engine::Side side, 
                         double price, uint64_t quantity) {
        uint64_t order_id = order_id_counter_++;
        matching_engine::Order order(order_id, client_id_, symbol, side, 
                                   matching_engine::OrderType::LIMIT, price, quantity);
        
        std::string order_msg = matching_engine::serialize_order(order);
        auto result = order_publication_->offer_with_retry(order_msg);
        
        std::cout << "Client " << client_id_ << " sent " 
                 << (side == matching_engine::Side::BUY ? "BUY" : "SELL")
                 << " LIMIT order: " << quantity << " " << symbol << " @ " 
                 << std::fixed << std::setprecision(2) << price 
                 << " (Result: " << aeron_wrapper::PublicationWrapper::result_to_string(result) << ")\n";
    }
    
    void send_market_order(const std::string& symbol, matching_engine::Side side, 
                          uint64_t quantity) {
        uint64_t order_id = order_id_counter_++;
        // Market orders typically use price 0 or a sentinel value
        matching_engine::Order order(order_id, client_id_, symbol, side, 
                                   matching_engine::OrderType::MARKET, 0.0, quantity);
        
        std::string order_msg = matching_engine::serialize_order(order);
        auto result = order_publication_->offer_with_retry(order_msg);
        
        std::cout << "Client " << client_id_ << " sent " 
                 << (side == matching_engine::Side::BUY ? "BUY" : "SELL")
                 << " MARKET order: " << quantity << " " << symbol 
                 << " (Result: " << aeron_wrapper::PublicationWrapper::result_to_string(result) << ")\n";
    }
    
    // Convenience method to send orders of any type
    void send_order(const std::string& symbol, matching_engine::Side side, 
                   matching_engine::OrderType type, double price, uint64_t quantity) {
        if (type == matching_engine::OrderType::MARKET) {
            send_market_order(symbol, side, quantity);
        } else {
            send_limit_order(symbol, side, price, quantity);
        }
    }
    
    void start_listening() {
        // Listen for trades
        auto trade_poller = trade_subscription_->start_background_polling(
            [this](const aeron_wrapper::FragmentData& fragment) {
                std::string message = fragment.as_string();
                if (message.substr(0, 6) == "TRADE|") {
                    std::cout << "Client " << client_id_ << " received trade: " << message << "\n";
                }
            });
        
        // Listen for acks
        auto ack_poller = ack_subscription_->start_background_polling(
            [this](const aeron_wrapper::FragmentData& fragment) {
                std::string message = fragment.as_string();
                if (message.substr(0, 4) == "ACK|") {
                    std::cout << "Client " << client_id_ << " received ack: " << message << "\n";
                }
            });
        
        // Keep pollers alive (in real implementation, you'd manage these properly)
        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
    
    // Helper methods for testing scenarios
    void send_aggressive_buy(const std::string& symbol, uint64_t quantity) {
        send_market_order(symbol, matching_engine::Side::BUY, quantity);
    }
    
    void send_aggressive_sell(const std::string& symbol, uint64_t quantity) {
        send_market_order(symbol, matching_engine::Side::SELL, quantity);
    }
    
    uint64_t get_client_id() const { return client_id_; }
    uint64_t get_next_order_id() const { return order_id_counter_.load(); }
};
