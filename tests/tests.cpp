#include "elevator/model.hpp"
#include "elevator/simulator.hpp"
#include "elevator/udp.hpp"
#include <atomic>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

using namespace elevator;
namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void rejects(const std::function<void()>& action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Expected invalid_argument");
}
Request request(int id, int pickup, int destination, Fault fault = Fault::None, int delay = 0) {
    Request r;
    r.id = id; r.pickup = pickup; r.destination = destination;
    r.direction = pickup <= destination ? Direction::Up : Direction::Down;
    r.fault = fault; r.fault_delay_ms = delay;
    return r;
}
Config fast_config(int elevators = 2) {
    Config c;
    c.floors = 22; c.elevators = elevators; c.time_scale = 0.001;
    c.timeout_ms = 10000; c.verbose = false;
    return c;
}
void csv_and_serialization() {
    std::istringstream csv("Time,Floor,Floor Button,Car Button,TimeError,Error\n"
                           "14:33:44.5,2,UP,4,0,null\n"
                           "14:33:44.050,4,DOWN,1,1000,DOOR_INTERRUPT\n");
    auto requests = read_requests(csv, 22);
    require(requests.size() == 2 && requests[0].id == 2, "CSV must sort by timestamp");
    require(requests[1].time_ms - requests[0].time_ms == 450, "Fractional seconds must be scaled");
    for (const auto& r : requests) require(serialize(deserialize(serialize(r), 22)) == serialize(r), "Wire round trip");
    rejects([] { deserialize("1 0 1 9 2 0 0", 22); });
    rejects([] { deserialize("1 0 1 0 2 0 0 junk", 22); });
    rejects([] { deserialize("1 0 23 1 2 0 0", 22); });
    rejects([] {
        std::istringstream bad("Time,Floor,Floor Button,Car Button,TimeError,Error\n14:00:00,3,UP,1,0,null\n");
        read_requests(bad, 22);
    });
    rejects([] {
        std::istringstream bad("Time,Floor,Floor Button,Car Button,TimeError,Error\n25:00:00,1,UP,2,0,null\n");
        read_requests(bad, 22);
    });
}
void clook_order() {
    CLook routing(5);
    std::vector<Request> pending{request(1, 2, 3), request(2, 8, 9), request(3, 6, 7), request(4, 1, 2)};
    std::vector<int> order;
    while (!pending.empty()) {
        const auto index = routing.next(pending);
        order.push_back(pending[index].pickup);
        pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(index));
    }
    require(order == std::vector<int>({6, 8, 1, 2}), "C-LOOK must sweep then wrap, without reversing");
    rejects([&] { routing.next(pending); });
    pending = {request(1, 2, 3), request(2, 2, 4)};
    require(routing.next(pending) == 0, "Same-floor calls retain arrival order");
}
void udp_round_trip() {
    UdpSocket first, second;
    require(!first.receive(1), "Receive timeout");
    first.send(second.port(), "hello");
    auto packet = second.receive(1000);
    require(packet && packet->body == "hello" && packet->port == first.port(), "Real UDP datagram");
    second.send(packet->port, "ACK");
    auto ack = first.receive(1000);
    require(ack && ack->body == "ACK", "UDP reply");
}
void rpc_retry() {
    UdpSocket server;
    std::atomic<bool> success{false};
    std::thread worker([&] {
        try {
            auto first = server.receive(2000);
            auto retry = server.receive(2000); // Simulate a dropped first response.
            if (!first || !retry || first->body != retry->body || first->port != retry->port) return;
            const auto separator = retry->body.find(' ');
            server.send(retry->port, retry->body.substr(0, separator) + " ACK");
            success = true;
        } catch (...) {}
    });
    std::string result;
    try { RpcClient client(server.port()); result = client.call("TEST"); }
    catch (...) { worker.join(); throw; }
    worker.join();
    require(success && result == "ACK", "RPC must retry with identical sequence number");
}
void concurrent_trips() {
    std::vector<Request> requests;
    for (int i = 1; i <= 24; ++i) requests.push_back(request(i, (i % 7) + 1, ((i + 3) % 7) + 1));
    auto result = simulate(requests, fast_config(4));
    require(result.submitted == 24 && result.completed == 24 && result.failed == 0, "Concurrent requests must all complete exactly once");
    auto same_floor = simulate({request(1, 1, 1)}, fast_config(1));
    require(same_floor.completed == 1, "Same-floor request must terminate");
    auto empty = simulate({}, fast_config());
    require(empty.submitted == 0 && empty.failed == 0, "Empty input must shut down cleanly");
}
void faults() {
    auto door = simulate({request(1, 1, 3, Fault::DoorInterrupt, 1000)}, fast_config(1));
    require(door.completed == 1 && door.door_interrupts == 1, "Door interruption must recover once");
    auto stuck = simulate({request(1, 1, 8, Fault::Stuck, 2000), request(2, 2, 3)}, fast_config(1));
    require(stuck.failed == 2 && stuck.completed == 0 && stuck.stuck_elevators == 1, "All-stuck fleet must account for pending passengers");
    auto recovery = simulate({request(1, 1, 8, Fault::Stuck, 2000), request(2, 2, 3), request(3, 3, 5)}, fast_config(2));
    require(recovery.failed == 1 && recovery.completed == 2, "Healthy elevator must continue serving pending calls");
}
void validation() {
    auto config = fast_config(); config.elevators = 0;
    rejects([&] { simulate({}, config); });
    rejects([] { simulate({request(1, 1, 3), request(1, 2, 4)}, fast_config()); });
    auto c = fast_config(); c.timeout_ms = 1; c.time_scale = 1;
    bool timed_out = false;
    try { simulate({request(1, 1, 22)}, c); }
    catch (const std::runtime_error&) { timed_out = true; }
    require(timed_out, "Watchdog must cancel and join workers");
}
}
int main(int argc, char** argv) {
    int failures = 0;
    const auto run = [&](const char* name, const std::function<void()>& action) {
        try { action(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };
    run("CSV and serialization", csv_and_serialization);
    run("C-LOOK ordering", clook_order);
    run("UDP sockets", udp_round_trip);
    run("RPC retry", rpc_retry);
    run("Concurrent trips and shutdown", concurrent_trips);
    run("Fault recovery", faults);
    run("Validation and watchdog", validation);
    run("Original CSV scenarios", [&] {
        require(argc == 2, "Resource directory required");
        for (int i = 1; i <= 7; ++i) {
            std::string path = std::string(argv[1]) + "/elevator_input" + (i == 1 ? "" : std::to_string(i)) + ".csv";
            std::ifstream input(path);
            require(static_cast<bool>(input), "Cannot read " + path);
            auto requests = read_requests(input, 22);
            const auto result = simulate(requests, fast_config());
            require(result.submitted == requests.size() && result.completed + result.failed == requests.size(), "Scenario lost requests: " + path);
        }
    });
    return failures == 0 ? 0 : 1;
}
