#include "elevator/simulator.hpp"
#include "elevator/udp.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace elevator {
namespace {
using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;
class Runtime {
public:
    explicit Runtime(const Config& config) : config(config), deadline(Clock::now() + Milliseconds(config.timeout_ms)) {}
    const Config& config;
    std::atomic<bool> cancel{false};
    Clock::time_point deadline;
    void pause(std::int64_t simulated_ms) {
        const auto end = Clock::now() + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double, std::milli>(static_cast<double>(simulated_ms) * config.time_scale));
        while (Clock::now() < end) {
            check();
            std::this_thread::sleep_for(std::min(Clock::duration(Milliseconds(2)), end - Clock::now()));
        }
        check();
    }
    void check() const {
        if (cancel.load()) throw std::runtime_error("Simulation cancelled");
        if (Clock::now() >= deadline) throw std::runtime_error("Simulation exceeded wall-clock timeout");
    }
    void log(const std::string& line) {
        if (config.verbose) { std::lock_guard<std::mutex> lock(mutex_); std::cout << line << '\n'; }
    }
    void guard(const std::function<void()>& action) noexcept {
        try { action(); }
        catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!error_) error_ = std::current_exception();
            cancel.store(true);
        }
    }
    void rethrow() { if (error_) std::rethrow_exception(error_); }
private:
    std::mutex mutex_;
    std::exception_ptr error_;
};
// Joins already-started threads even if a later thread cannot be created.
class Workers {
public:
    explicit Workers(Runtime& runtime) : runtime_(runtime) {}
    ~Workers() { runtime_.cancel.store(true); join(); }
    void start(std::function<void()> action) {
        threads_.emplace_back([this, action = std::move(action)] { runtime_.guard(action); });
    }
    void join() { for (auto& t : threads_) if (t.joinable()) t.join(); }
private:
    Runtime& runtime_;
    std::vector<std::thread> threads_;
};

enum class StateKind { Idle, Moving, Opening, Boarding, Closing, Interrupted, Stuck };
class Elevator;
class ElevatorState {
public:
    virtual ~ElevatorState() = default;
    virtual StateKind kind() const = 0;
    virtual const char* name() const = 0;
    virtual void step(Elevator&) const = 0;
};
const ElevatorState& state_for(StateKind kind);
class Elevator {
public:
    Elevator(int id, std::uint16_t server, Runtime& runtime)
        : id(id), client(server), runtime(runtime), state_(&state_for(StateKind::Idle)) {}
    int id;
    int floor = 1;
    Request trip;
    RpcClient client;
    Runtime& runtime;
    bool running = true;
    bool picked_up = false;
    bool door_fault_used = false;
    bool just_picked_up = false;
    std::int64_t since_pickup = 0;
    void transition(StateKind kind) {
        state_ = &state_for(kind);
        runtime.log("Elevator " + std::to_string(id + 1) + " floor " + std::to_string(floor) + " " + state_->name());
    }
    void run() { while (running) { runtime.check(); state_->step(*this); } }
    // Fault countdown uses simulated time and interrupts an action at its deadline.
    bool delay(int milliseconds) {
        if (picked_up && trip.fault == Fault::Stuck && since_pickup + milliseconds >= trip.fault_delay_ms) {
            runtime.pause(std::max<std::int64_t>(0, trip.fault_delay_ms - since_pickup));
            transition(StateKind::Stuck);
            return false;
        }
        runtime.pause(milliseconds);
        if (picked_up) since_pickup += milliseconds;
        return true;
    }
    void complete() {
        client.call("COMPLETE " + std::to_string(id) + " " + std::to_string(trip.id));
        transition(StateKind::Idle);
    }
private:
    const ElevatorState* state_;
};
class IdleState final : public ElevatorState {
public:
    StateKind kind() const override { return StateKind::Idle; }
    const char* name() const override { return "IDLE"; }
    void step(Elevator& e) const override {
        auto reply = e.client.call("POLL " + std::to_string(e.id) + " " + std::to_string(e.floor));
        if (reply == "STOP") { e.running = false; return; }
        if (reply == "WAIT") { std::this_thread::sleep_for(Milliseconds(1)); return; }
        if (reply.compare(0, 5, "TRIP ") != 0) throw std::runtime_error("Invalid assignment");
        e.trip = deserialize(reply.substr(5), e.runtime.config.floors);
        e.picked_up = e.door_fault_used = e.just_picked_up = false;
        e.since_pickup = 0;
        e.transition(e.floor == e.trip.pickup ? StateKind::Opening : StateKind::Moving);
    }
};
class MovingState final : public ElevatorState {
public:
    StateKind kind() const override { return StateKind::Moving; }
    const char* name() const override { return "MOVING"; }
    void step(Elevator& e) const override {
        const int target = e.picked_up ? e.trip.destination : e.trip.pickup;
        if (target == e.floor) { e.transition(StateKind::Opening); return; }
        if (!e.delay(e.runtime.config.move_ms)) return;
        e.floor += target > e.floor ? 1 : -1;
        if (e.floor == target) e.transition(StateKind::Opening);
    }
};
class OpeningState final : public ElevatorState {
public:
    StateKind kind() const override { return StateKind::Opening; }
    const char* name() const override { return "OPENING"; }
    void step(Elevator& e) const override {
        if (!e.delay(e.runtime.config.door_ms)) return;
        e.just_picked_up = !e.picked_up;
        e.picked_up = true;
        e.transition(StateKind::Boarding);
    }
};
class BoardingState final : public ElevatorState {
public:
    StateKind kind() const override { return StateKind::Boarding; }
    const char* name() const override { return "BOARDING"; }
    void step(Elevator& e) const override {
        if (e.just_picked_up && e.trip.fault == Fault::DoorInterrupt && !e.door_fault_used) {
            if (e.trip.fault_delay_ms <= e.runtime.config.boarding_ms) {
                if (!e.delay(e.trip.fault_delay_ms)) return;
                e.door_fault_used = true;
                e.transition(StateKind::Interrupted);
                return;
            }
            // A door interrupt outside boarding cannot interrupt the doors.
            e.door_fault_used = true;
        }
        if (!e.delay(e.runtime.config.boarding_ms)) return;
        e.transition(StateKind::Closing);
    }
};
class ClosingState final : public ElevatorState {
public:
    StateKind kind() const override { return StateKind::Closing; }
    const char* name() const override { return "CLOSING"; }
    void step(Elevator& e) const override {
        if (!e.delay(e.runtime.config.door_ms)) return;
        if (e.floor == e.trip.destination) e.complete();
        else e.transition(StateKind::Moving);
    }
};
class InterruptedState final : public ElevatorState {
public:
    StateKind kind() const override { return StateKind::Interrupted; }
    const char* name() const override { return "DOOR_INTERRUPT"; }
    void step(Elevator& e) const override {
        e.client.call("DOOR " + std::to_string(e.id) + " " + std::to_string(e.trip.id));
        if (e.delay(e.runtime.config.door_ms)) e.transition(StateKind::Boarding);
    }
};
class StuckState final : public ElevatorState {
public:
    StateKind kind() const override { return StateKind::Stuck; }
    const char* name() const override { return "STUCK"; }
    void step(Elevator& e) const override {
        e.client.call("STUCK " + std::to_string(e.id) + " " + std::to_string(e.trip.id));
        e.running = false;
    }
};
const ElevatorState& state_for(StateKind kind) {
    static IdleState idle;
    static MovingState moving;
    static OpeningState opening;
    static BoardingState boarding;
    static ClosingState closing;
    static InterruptedState interrupted;
    static StuckState stuck;
    switch (kind) {
    case StateKind::Idle: return idle;
    case StateKind::Moving: return moving;
    case StateKind::Opening: return opening;
    case StateKind::Boarding: return boarding;
    case StateKind::Closing: return closing;
    case StateKind::Interrupted: return interrupted;
    case StateKind::Stuck: return stuck;
    }
    throw std::logic_error("Unknown state");
}

class Scheduler {
public:
    explicit Scheduler(Runtime& runtime) : runtime_(runtime) {}
    std::uint16_t port() const { return socket_.port(); }
    Result result;
    void run() {
        while (!runtime_.cancel.load()) {
            runtime_.check();
            auto packet = socket_.receive(20);
            if (!packet) continue;
            const auto separator = packet->body.find(' ');
            if (separator == std::string::npos) continue;
            const auto token = packet->body.substr(0, separator);
            std::uint64_t sequence;
            try {
                std::size_t consumed;
                sequence = std::stoull(token, &consumed);
                if (consumed != token.size() || sequence == 0) continue;
            } catch (const std::exception&) { continue; }
            auto cached = replies_.find(packet->port);
            if (cached != replies_.end() && cached->second.first >= sequence) {
                if (cached->second.first == sequence) socket_.send(packet->port, cached->second.second);
                continue;
            }
            std::string reply;
            try { reply = process(packet->body.substr(separator + 1)); }
            catch (const std::invalid_argument& e) { reply = std::string("ERROR ") + e.what(); }
            const auto response = token + " " + reply;
            replies_[packet->port] = {sequence, response};
            socket_.send(packet->port, response);
        }
    }
private:
    Runtime& runtime_;
    UdpSocket socket_;
    CLook routing_;
    std::vector<Request> pending_;
    std::map<int, Request> active_;
    std::set<int> ids_;
    std::set<int> stuck_;
    bool floor_done_ = false;
    std::map<std::uint16_t, std::pair<std::uint64_t, std::string>> replies_;
    static void end(std::istringstream& in) {
        in >> std::ws;
        if (!in.eof()) throw std::invalid_argument("Trailing protocol fields");
    }
    std::string process(const std::string& body) {
        if (body.compare(0, 8, "REQUEST ") == 0) {
            if (floor_done_) throw std::invalid_argument("Floor input already closed");
            auto request = deserialize(body.substr(8), runtime_.config.floors);
            if (ids_.insert(request.id).second) {
                pending_.push_back(request);
                ++result.submitted;
            }
            return "ACK";
        }
        if (body == "DONE") { floor_done_ = true; return "ACK"; }
        std::istringstream in(body);
        std::string operation;
        int id, value;
        if (!(in >> operation >> id >> value) || id < 0 || id >= runtime_.config.elevators)
            throw std::invalid_argument("Invalid elevator packet");
        end(in);
        if (operation == "POLL") {
            if (value < 1 || value > runtime_.config.floors || active_.count(id) || stuck_.count(id))
                throw std::invalid_argument("Invalid idle elevator");
            if (!pending_.empty()) {
                const auto index = routing_.next(pending_);
                auto request = pending_[index];
                pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
                active_[id] = request;
                runtime_.log("Assigned request " + std::to_string(request.id) + " pickup " + std::to_string(request.pickup) + " to elevator " + std::to_string(id + 1));
                return "TRIP " + serialize(request);
            }
            return floor_done_ && active_.empty() ? "STOP" : "WAIT";
        }
        auto active = active_.find(id);
        if (active == active_.end() || active->second.id != value) throw std::invalid_argument("Unknown active trip");
        if (operation == "DOOR") { ++result.door_interrupts; return "ACK"; }
        if (operation == "COMPLETE") {
            ++result.completed;
        } else if (operation == "STUCK") {
            stuck_.insert(id);
            ++result.stuck_elevators;
            ++result.failed; // Onboard passengers require external rescue.
        } else throw std::invalid_argument("Unknown operation");
        active_.erase(active);
        return "ACK";
    }
public:
    // Called after all workers join; no shared mutable scheduler access.
    void finish() {
        result.failed += pending_.size();
        pending_.clear();
        if (result.completed + result.failed != result.submitted)
            throw std::runtime_error("Simulation ended with unaccounted requests");
    }
};
}

Result simulate(const std::vector<Request>& requests, const Config& config) {
    if (config.floors < 1 || config.elevators < 1 || config.elevators > 128 ||
        !std::isfinite(config.time_scale) || config.time_scale <= 0 || config.time_scale > 100 ||
        config.move_ms < 1 || config.door_ms < 1 || config.boarding_ms < 1 || config.timeout_ms < 1)
        throw std::invalid_argument("Invalid simulator configuration");
    std::set<int> ids;
    for (const auto& request : requests) {
        validate(request, config.floors);
        if (!ids.insert(request.id).second) throw std::invalid_argument("Duplicate request ID");
    }
    if (!std::is_sorted(requests.begin(), requests.end(), [](const Request& a, const Request& b) { return a.time_ms < b.time_ms; }))
        throw std::invalid_argument("Requests must be ordered by timestamp");
    Runtime runtime(config);
    Scheduler scheduler(runtime);
    Workers server(runtime);
    server.start([&] { scheduler.run(); });
    // Construct clients before spawning threads, ensuring RAII on startup failure.
    std::vector<std::unique_ptr<Elevator>> elevators;
    for (int id = 0; id < config.elevators; ++id)
        elevators.push_back(std::make_unique<Elevator>(id, scheduler.port(), runtime));
    Workers workers(runtime);
    const auto start = Clock::now();
    workers.start([&] {
        RpcClient floor(scheduler.port());
        const auto base = requests.empty() ? 0 : requests.front().time_ms;
        for (const auto& request : requests) {
            const auto due = start + std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double, std::milli>(static_cast<double>(request.time_ms - base) * config.time_scale));
            while (Clock::now() < due) {
                runtime.check();
                std::this_thread::sleep_for(Milliseconds(1));
            }
            floor.call("REQUEST " + serialize(request));
        }
        floor.call("DONE");
    });
    for (const auto& elevator : elevators) {
        auto* worker = elevator.get();
        workers.start([worker] { worker->run(); });
    }
    workers.join();
    runtime.cancel.store(true);
    server.join();
    runtime.rethrow();
    scheduler.finish();
    return scheduler.result;
}
}
