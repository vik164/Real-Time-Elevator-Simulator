#pragma once
#include "elevator/model.hpp"
#include <cstddef>
#include <vector>

namespace elevator {
struct Config {
    int floors = 22;
    int elevators = 4;
    double time_scale = 1.0; // wall milliseconds per simulated millisecond
    int move_ms = 5000;
    int door_ms = 1000;
    int boarding_ms = 3000;
    int timeout_ms = 600000; // wall-clock watchdog
    bool verbose = true;
};
struct Result {
    std::size_t submitted = 0;
    std::size_t completed = 0;
    std::size_t failed = 0;
    std::size_t door_interrupts = 0;
    std::size_t stuck_elevators = 0;
};
// Runs a floor producer, scheduler server, and one worker per elevator.
// Every request, assignment, and completion crosses a real loopback UDP socket.
Result simulate(const std::vector<Request>& requests, const Config& config = {});
}
