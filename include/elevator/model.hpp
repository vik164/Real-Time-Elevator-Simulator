#pragma once
#include <cstdint>
#include <istream>
#include <string>
#include <vector>

namespace elevator {
enum class Direction { Up, Down };
enum class Fault { None, DoorInterrupt, Stuck };
struct Request {
    int id = 0;
    std::int64_t time_ms = 0;
    int pickup = 1;
    Direction direction = Direction::Up;
    int destination = 1;
    int fault_delay_ms = 0;
    Fault fault = Fault::None;
};
void validate(const Request& request, int floors);
std::vector<Request> read_requests(std::istream& input, int floors);
std::string serialize(const Request& request);
Request deserialize(const std::string& text, int floors);

// Circular LOOK over pending pickup floors: ascending sweep, then wrap to
// the lowest pending pickup. The cursor never visits an unused boundary floor.
class CLook {
public:
    explicit CLook(int cursor = 1) : cursor_(cursor) {}
    std::size_t next(const std::vector<Request>& requests);
    int cursor() const { return cursor_; }
private:
    int cursor_;
};
}
