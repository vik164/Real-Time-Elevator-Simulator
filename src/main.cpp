#include "elevator/simulator.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
int integer(const std::string& text) {
    std::size_t used;
    const auto value = std::stoi(text, &used);
    if (used != text.size()) throw std::invalid_argument("Invalid integer: " + text);
    return value;
}
}
int main(int argc, char** argv) {
    try {
        elevator::Config config;
        std::string input = "resources/elevator_input6.csv";
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") {
                std::cout << "Usage: elevator_simulator [--input FILE] [--floors N] [--elevators N]\n"
                             "       [--time-scale FACTOR] [--timeout-ms N] [--quiet]\n";
                return 0;
            }
            if (option == "--quiet") { config.verbose = false; continue; }
            if (i + 1 == argc) throw std::invalid_argument("Missing value for " + option);
            const std::string value = argv[++i];
            if (option == "--input") input = value;
            else if (option == "--floors") config.floors = integer(value);
            else if (option == "--elevators") config.elevators = integer(value);
            else if (option == "--timeout-ms") config.timeout_ms = integer(value);
            else if (option == "--time-scale") {
                std::size_t used;
                config.time_scale = std::stod(value, &used);
                if (used != value.size()) throw std::invalid_argument("Invalid time scale");
            } else throw std::invalid_argument("Unknown option: " + option);
        }
        std::ifstream file(input);
        if (!file) throw std::runtime_error("Cannot open input: " + input);
        auto requests = elevator::read_requests(file, config.floors);
        const auto result = elevator::simulate(requests, config);
        std::cout << "Submitted: " << result.submitted << " Completed: " << result.completed
                  << " Failed: " << result.failed << " Door interrupts: " << result.door_interrupts
                  << " Stuck elevators: " << result.stuck_elevators << '\n';
        return result.failed == 0 ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
