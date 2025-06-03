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