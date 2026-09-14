// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// Original, transport-independent code. Does not speak the upstream NVIDIA
// emulation protocol, drive GPIO, or claim calibrated optical timing.
namespace vision::rp2040 {
constexpr size_t packetSize = 64, payloadCapacity = 32;
constexpr uint8_t protocolVersion = 1;
enum class Opcode : uint8_t { Hello=1, Clock=2, Configure=3, Schedule=4, Stop=5, Status=6, Diagnostics=7, Reply=128 };
enum class Result : uint8_t {
    Ok, BadPacket, BadVersion, BadOpcode, BadLength, BadChecksum,
    BadSession, BadConfig, NotReady, StaleSequence, QueueFull,
    TooSoon, TooFar, Overlap, WrongEye, Late, Timeout, ClockReversed, Stopped
};
struct Message {
    Opcode opcode=Opcode::Hello;
    uint64_t session=0;
    uint32_t request=0;
    uint16_t length=0;
    std::array<uint8_t,payloadCapacity> payload{};
};
using Packet = std::array<uint8_t,packetSize>;
std::optional<Packet> encode(const Message& message);
Result decode(std::span<const uint8_t> bytes, Message& message);
uint32_t checksum(std::span<const uint8_t> bytes);
void put32(std::span<uint8_t> bytes, uint32_t value);
void put64(std::span<uint8_t> bytes, uint64_t value);
uint32_t get32(std::span<const uint8_t> bytes);
uint64_t get64(std::span<const uint8_t> bytes);

enum class Eye : uint8_t { Left=0, Right=1 };
struct Config { uint32_t periodUs=8333, leftUs=1500, rightUs=1500; };
struct Frame { uint64_t session=0, sequence=0, openUs=0; Eye eye=Eye::Left; };
enum class ActionKind : uint8_t { Open, Close };
struct Action { ActionKind kind; Eye eye; uint64_t sequence, scheduledUs; };
struct Step {
    std::array<Action,32> actions{};
    size_t count=0;
    bool forceIdle=false;
    Result reason=Result::Ok;
};

// Fixed-capacity reference scheduler. A hardware adapter must implement
// forceIdle and translate abstract lens commands into verified IR waveforms.
class Scheduler {
public:
    static constexpr size_t capacity=16;
    static constexpr uint64_t minLeadUs=500, horizonUs=50000;
    static constexpr uint64_t maxLateUs=100, watchdogUs=100000, guardUs=250;
    Result begin(uint64_t session, uint64_t now);
    Result configure(uint64_t session, Config config, uint64_t now);
    Result enqueue(Frame frame, uint64_t now);
    Step advance(uint64_t now);
    Step stop(Result reason=Result::Stopped);
    bool ready() const { return configured_ && session_!=0; }
    size_t queued() const { return count_; }
    uint64_t session() const { return session_; }
    uint64_t lastSession() const { return lastSession_; }
    uint64_t rejected() const { return rejected_; }
private:
    struct Entry { Frame frame{}; uint64_t closeUs=0; bool opened=false; };
    std::array<Entry,capacity> queue_{};
    size_t head_=0, count_=0;
    uint64_t session_=0, lastSession_=0, lastNow_=0, lastActivity_=0;
    uint64_t lastSequence_=0, lastClose_=0, lastOpen_=0, rejected_=0;
    Eye lastEye_=Eye::Right;
    Config config_{};
    bool configured_=false, hadFrame_=false;
    Result reject(Result result) { ++rejected_; return result; }
};

struct ClockEstimate { double deviceUs=0, uncertaintyUs=0; };
// Interval estimator: deliberately does not assume symmetric USB latency.
// Values are microseconds. The drift bound is an engineering assumption to
// verify on hardware, not a measured RP2040 oscillator specification.
class ClockMap {
public:
    static constexpr double driftPpm=1000, maxAgeUs=1000000, maxUncertaintyUs=250;
    bool observe(double hostSend, double deviceReceive, double deviceSend, double hostReceive);
    std::optional<ClockEstimate> estimate(double hostTarget, double hostNow) const;
    void reset();
    unsigned samples() const { return samples_; }
private:
    double low_=0, high_=0, anchor_=0, lastReceive_=0;
    unsigned samples_=0;
};

struct Exchange {
    std::optional<Packet> reply;
    bool forceIdle=false;
};
// In-process device endpoint for host integration tests. A future USB adapter
// must service scheduler.advance independently of USB traffic and honor idle.
// Replies explicitly identify this implementation as a simulation.
class DeviceEndpoint {
public:
    explicit DeviceEndpoint(uint64_t bootId,bool simulated=true) : bootId_(bootId), simulated_(simulated) {}
    Exchange receive(std::span<const uint8_t> packet,uint64_t receiveUs,uint64_t replyUs);
    Step advance(uint64_t now);
    Step disconnect() { return scheduler_.stop(); }
private:
    Scheduler scheduler_;
    uint64_t bootId_;
    bool simulated_;
    uint64_t opens_=0, closes_=0;
    Result lastFault_=Result::Ok;
};
class SimulatedDevice : public DeviceEndpoint {
public: explicit SimulatedDevice(uint64_t bootId) : DeviceEndpoint(bootId,true) {}
};
}
