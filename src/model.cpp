#include "elevator/model.hpp"
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace elevator {
namespace {
int integer(const std::string& text) {
    std::size_t consumed = 0;
    int value = std::stoi(text, &consumed);
    if (consumed != text.size()) throw std::invalid_argument("Invalid integer: " + text);
    return value;
}
std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> fields;
    std::istringstream stream(text);
    std::string field;
    while (std::getline(stream, field, separator)) {
        const auto first = field.find_first_not_of(" \t\r");
        const auto last = field.find_last_not_of(" \t\r");
        fields.push_back(first == std::string::npos ? "" : field.substr(first, last - first + 1));
    }
    return fields;
}
std::int64_t timestamp(const std::string& text) {
    auto parts = split(text, ':');
    if (parts.size() != 3) throw std::invalid_argument("Expected HH:mm:ss[.SSS]");
    const int hour = integer(parts[0]), minute = integer(parts[1]);
    auto seconds = split(parts[2], '.');
    if (seconds.empty() || seconds.size() > 2) throw std::invalid_argument("Invalid seconds");
    const int second = integer(seconds[0]);
    std::string fraction = seconds.size() == 2 ? seconds[1] : "0";
    if (fraction.empty() || fraction.size() > 3) throw std::invalid_argument("Invalid milliseconds");
    fraction.append(3 - fraction.size(), '0');
    const int ms = integer(fraction);
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59 || ms < 0 || ms > 999)
        throw std::invalid_argument("Timestamp out of range");
    return ((hour * 60LL + minute) * 60 + second) * 1000 + ms;
}
}
void validate(const Request& r, int floors) {
    if (r.id < 0 || r.time_ms < 0 || r.pickup < 1 || r.pickup > floors || r.destination < 1 || r.destination > floors || r.fault_delay_ms < 0)
        throw std::invalid_argument("Request value out of range");
    if ((r.direction != Direction::Up && r.direction != Direction::Down) ||
        (r.fault != Fault::None && r.fault != Fault::DoorInterrupt && r.fault != Fault::Stuck))
        throw std::invalid_argument("Invalid request enum");
    if ((r.destination > r.pickup && r.direction != Direction::Up) ||
        (r.destination < r.pickup && r.direction != Direction::Down))
        throw std::invalid_argument("Direction disagrees with destination");
}
std::vector<Request> read_requests(std::istream& input, int floors) {
    std::vector<Request> result;
    std::string line;
    if (!std::getline(input, line) || split(line, ',') != std::vector<std::string>{"Time", "Floor", "Floor Button", "Car Button", "TimeError", "Error"})
        throw std::invalid_argument("Invalid CSV header");
    int line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        try {
            auto fields = split(line, ',');
            if (fields.size() != 6) throw std::invalid_argument("Expected six columns");
            Request r;
            r.id = static_cast<int>(result.size()) + 1;
            r.time_ms = timestamp(fields[0]);
            r.pickup = integer(fields[1]);
            if (fields[2] != "UP" && fields[2] != "DOWN") throw std::invalid_argument("Invalid direction");
            r.direction = fields[2] == "UP" ? Direction::Up : Direction::Down;
            r.destination = integer(fields[3]);
            r.fault_delay_ms = integer(fields[4]);
            if (fields[5] == "null" || fields[5] == "NONE") r.fault = Fault::None;
            else if (fields[5] == "DOOR_INTERRUPT") r.fault = Fault::DoorInterrupt;
            else if (fields[5] == "ELEVATOR_STUCK") r.fault = Fault::Stuck;
            else throw std::invalid_argument("Invalid fault");
            validate(r, floors);
            result.push_back(r);
        } catch (const std::exception& e) {
            throw std::invalid_argument("CSV line " + std::to_string(line_number) + ": " + e.what());
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const Request& a, const Request& b) { return a.time_ms < b.time_ms; });
    return result;
}
std::string serialize(const Request& r) {
    std::ostringstream out;
    out << r.id << ' ' << r.time_ms << ' ' << r.pickup << ' ' << static_cast<int>(r.direction)
        << ' ' << r.destination << ' ' << r.fault_delay_ms << ' ' << static_cast<int>(r.fault);
    return out.str();
}
Request deserialize(const std::string& text, int floors) {
    Request r;
    int direction, fault;
    std::istringstream in(text);
    if (!(in >> r.id >> r.time_ms >> r.pickup >> direction >> r.destination >> r.fault_delay_ms >> fault))
        throw std::invalid_argument("Malformed request packet");
    in >> std::ws;
    if (!in.eof() || direction < 0 || direction > 1 || fault < 0 || fault > 2)
        throw std::invalid_argument("Invalid request packet");
    r.direction = static_cast<Direction>(direction);
    r.fault = static_cast<Fault>(fault);
    validate(r, floors);
    return r;
}
std::size_t CLook::next(const std::vector<Request>& requests) {
    if (requests.empty()) throw std::invalid_argument("C-LOOK requires pending requests");
    std::size_t best = requests.size();
    for (std::size_t i = 0; i < requests.size(); ++i) {
        if (requests[i].pickup >= cursor_ &&
            (best == requests.size() || requests[i].pickup < requests[best].pickup)) best = i;
    }
    if (best == requests.size()) {
        best = static_cast<std::size_t>(std::min_element(requests.begin(), requests.end(),
            [](const Request& a, const Request& b) { return a.pickup < b.pickup; }) - requests.begin());
    }
    cursor_ = requests[best].pickup;
    return best;
}
}
