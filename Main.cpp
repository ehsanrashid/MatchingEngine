// main.cpp
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

/*
// Aeron includes
#include "types.h" // Include the types header for MessageType, Side, Order, etc.
#include "server.h"
#include "client.h"

int main(int argc, char* argv[]) {
    if (argc < 2) { 
        std::cout << "Usage: " << argv[0] << " [server|client]" << std::endl; 
        return 1; 
    }
    
    std::string mode(argv[1]);
    try {
        if (mode == "server") {
            std::cout << "Starting server..." << std::endl;
            MatchingEngineServer server(1001, 1002, 1000);
            std::cout << "Server ready. Press Enter to exit." << std::endl;
            std::cin.get();
        } else if (mode == "client") {
            std::cout << "Starting client..." << std::endl;
            MatchingEngineClient client(1000, 1001, 1002);

            // Give some time for connections to establish
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            std::cout << "Sending test orders..." << std::endl;
            client.sendOrder(1, 1, Side::BUY, 100, 1000);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            client.sendOrder(2, 1, Side::SELL, 50, 900);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            client.cancelOrder(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            std::cout << "Orders sent. Press Enter to exit." << std::endl;
            std::cin.get();
        } else {
            std::cout << "Invalid mode. Use 'server' or 'client'" << std::endl;
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
*/

/*
#include "aeronWrapper.h" // Include the Aeron wrapper header

int main() {
    std::cout << "Starting Aeron example..." << std::endl;
    try {
        // Create Aeron client
        aeron_wrapper::AeronClient client("");
        std::cout << "Aeron client initialized successfully!" << std::endl;

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

/*
#include <iostream>
#include <iomanip>
#include <sstream>

#include "aeronMatching.h"

void print_order_response(const matching_engine::MessageHeader* header, const std::uint8_t* buffer) {
    switch (header->msg_type) {
        case matching_engine::MessageType::ORDER_ACK: {
            const auto* ack = reinterpret_cast<const matching_engine::OrderAckMessage*>(buffer);
            std::cout << "Order ACK - ID: " << ack->order_id 
                     << ", Status: " << static_cast<int>(ack->status)
                     << ", Symbol: " << std::string(ack->symbol)
                     << ", Client: " << ack->client_id << std::endl;
            break;
        }
        case matching_engine::MessageType::FILL_REPORT: {
            const auto* fill = reinterpret_cast<const matching_engine::FillReportMessage*>(buffer);
            std::cout << "Fill Report - Order ID: " << fill->order_id
                     << ", Fill ID: " << fill->fill_id
                     << ", Symbol: " << std::string(fill->symbol)
                     << ", Side: " << (fill->side == matching_engine::OrderSide::BUY ? "BUY" : "SELL")
                     << ", Qty: " << fill->fill_quantity
                     << ", Price: " << fill->fill_price
                     << ", Leaves: " << fill->leaves_quantity << std::endl;
            break;
        }
        case matching_engine::MessageType::CANCEL_ACK: {
            const auto* ack = reinterpret_cast<const matching_engine::OrderAckMessage*>(buffer);
            std::cout << "Cancel ACK - Order ID: " << ack->order_id
                     << ", Symbol: " << std::string(ack->symbol) << std::endl;
            break;
        }
        default:
            std::cout << "Unknown message type: " << static_cast<int>(header->msg_type) << std::endl;
            break;
    }
}

int main() {
    try {
        // Create matching engine
        std::cout << "Starting Matching Engine..." << std::endl;

        std::string tmpDir = ""; //"/tmp/aeron";
        matching_engine::MatchingEngine engine(tmpDir);
        std::cout << "Started Matching Engine..." << std::endl;
        
        // Add symbols
        engine.add_symbol("AAPL");
        engine.add_symbol("MSFT");
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        
        // Create trading clients
        std::cout << "Creating trading clients..." << std::endl;
        matching_engine::TradingClient client1(1001, tmpDir);
        matching_engine::TradingClient client2(1002, tmpDir);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        
        std::cout << "Starting trading simulation..." << std::endl;
        
        // Client 1 places buy orders
        std::cout << "\n--- Client 1 placing buy orders ---" << std::endl;
        client1.send_order("AAPL", matching_engine::OrderSide::BUY, matching_engine::OrderType::LIMIT, 100, 15000); // $150.00
        client1.send_order("AAPL", matching_engine::OrderSide::BUY, matching_engine::OrderType::LIMIT, 200, 14950); // $149.50
        
        // Client 2 places sell orders
        std::cout << "\n--- Client 2 placing sell orders ---" << std::endl;
        client2.send_order("AAPL", matching_engine::OrderSide::SELL, matching_engine::OrderType::LIMIT, 150, 15050); // $150.50
        client2.send_order("AAPL", matching_engine::OrderSide::SELL, matching_engine::OrderType::LIMIT, 100, 15100); // $151.00
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Process responses
        std::cout << "\n--- Processing responses ---" << std::endl;
        for (int i = 0; i < 10; ++i) {
            client1.poll_responses(print_order_response);
            client2.poll_responses(print_order_response);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        // Check market data
        std::cout << "\n--- Market Data ---" << std::endl;
        client1.poll_market_data(matching_engine::utils::print_market_data);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Client 1 places aggressive order that will match
        std::cout << "\n--- Client 1 placing aggressive buy order ---" << std::endl;
        client1.send_order("AAPL", matching_engine::OrderSide::BUY, matching_engine::OrderType::LIMIT, 75, 15050); // Will match at $150.50
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Process fill reports
        std::cout << "\n--- Processing fill reports ---" << std::endl;
        for (int i = 0; i < 20; ++i) {
            client1.poll_responses(print_order_response);
            client2.poll_responses(print_order_response);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        // Check updated market data
        std::cout << "\n--- Updated Market Data ---" << std::endl;
        client1.poll_market_data(matching_engine::utils::print_market_data);
        
        // Market order test
        std::cout << "\n--- Client 2 placing market sell order ---" << std::endl;
        client2.send_order("AAPL", matching_engine::OrderSide::SELL, matching_engine::OrderType::MARKET, 50);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Process market order responses
        std::cout << "\n--- Processing market order responses ---" << std::endl;
        for (int i = 0; i < 10; ++i) {
            client1.poll_responses(print_order_response);
            client2.poll_responses(print_order_response);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        // Final market data
        std::cout << "\n--- Final Market Data ---" << std::endl;
        client1.poll_market_data(matching_engine::utils::print_market_data);
        
        std::cout << "\n=== Trading Simulation Complete ===" << std::endl;
        
        // Keep running for a bit to see market data updates
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        std::cout << "Shutting down..." << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
*/


#include "aeronMatching2.h"

// Main function demonstrating the matching engine
int main() {
    try {
        // Start matching engine
        matching_engine::MatchingEngine engine("");
        engine.start();
        
        // Give engine time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Create test clients
        TestClient client1(1001);
        TestClient client2(1002);
        TestClient client3(1003);
        
        // Start listening for responses
        std::thread listener1([&client1]() { client1.start_listening(); });
        std::thread listener2([&client2]() { client2.start_listening(); });
        std::thread listener3([&client3]() { client3.start_listening(); });
        
        // Give clients time to connect
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        std::cout << "\n=== Starting Trading Simulation ===\n";
        
        // Create some limit orders first to establish market
        client1.send_limit_order("AAPL", matching_engine::Side::BUY, 150.00, 100);
        client1.send_limit_order("AAPL", matching_engine::Side::BUY, 149.50, 200);
        client2.send_limit_order("AAPL", matching_engine::Side::SELL, 150.50, 100);
        client2.send_limit_order("AAPL", matching_engine::Side::SELL, 151.00, 200);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Now send market orders that should match
        std::cout << "\n--- Sending Market Orders ---\n";
        client1.send_market_order("AAPL", matching_engine::Side::BUY, 50);   // Should hit best ask
        client2.send_market_order("AAPL", matching_engine::Side::SELL, 75);  // Should hit best bid
        
        // Alternative using convenience methods
        client1.send_aggressive_buy("AAPL", 25);
        client2.send_aggressive_sell("AAPL", 50);
        
        // Wait for processing
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        // Send some orders
        client1.send_limit_order("AAPL", matching_engine::Side::BUY, 150.00, 100);
        client1.send_limit_order("AAPL", matching_engine::Side::BUY, 149.50, 200);
        
        client2.send_limit_order("AAPL", matching_engine::Side::SELL, 151.00, 150);
        client2.send_limit_order("AAPL", matching_engine::Side::SELL, 150.75, 100);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        engine.print_all_books();
        
        // Match some orders
        client3.send_limit_order("AAPL", matching_engine::Side::BUY, 151.00, 75);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        engine.print_all_books();
        
        client3.send_limit_order("AAPL", matching_engine::Side::SELL, 149.50, 50);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        engine.print_all_books();
        
        std::cout << "\n=== Trading Simulation Complete ===\n";
        
        // Wait a bit more for any remaining messages
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        engine.stop();
        // Note: In a real implementation, you'd properly join threads
        // Clean up threads (in real code, use proper thread management)
        listener1.join();
        listener2.join();
        listener3.join();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
