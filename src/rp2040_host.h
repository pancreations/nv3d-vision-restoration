// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "rp2040_protocol.h"
#include <functional>
#include <stop_token>
#include <string>
#include <stdexcept>
namespace vision::rp2040 {
class Link {
public:
    virtual ~Link()=default;
    virtual Packet exchange(const Packet&,std::stop_token)=0;
    virtual std::string identity() const { return "test-link"; }
    virtual std::string firmwareVersion() const { return "test-firmware"; }
};
struct HostStatus {
    uint64_t bootId=0, session=0, lastEpoch=0, events=0;
    double roundTripUs=0, uncertaintyUs=0;
    bool simulated=false, clockReady=false, extendedCadence=false, aperture=false, fastCadence=false;
};
class ScheduleMiss : public std::runtime_error { public: using std::runtime_error::runtime_error; };
class InvalidTiming : public std::invalid_argument { public: using std::invalid_argument::invalid_argument; };
// Run the device's own configuration checks before touching a live session.
bool validConfig(Config config);
class Client {
public:
    Client(Link& link,std::function<double()> hostMicroseconds,bool allowSimulation=false);
    void inspect(std::stop_token stop={});
    bool sampleClock(std::stop_token stop={});
    void start(Config config,std::stop_token stop={});
    void schedule(Eye eye,double hostTargetUs,std::stop_token stop={});
    void stop(std::stop_token token={});
    Message diagnostics(std::stop_token stop={});
    HostStatus status() const { return status_; }
private:
    Link& link_;std::function<double()> now_;bool allowSimulation_;
    HostStatus status_;ClockMap clock_;uint32_t request_=0;uint64_t sequence_=0;
    Message transact(Opcode op,std::span<const uint8_t> payload,std::stop_token stop);
};
const char* resultName(Result result);
}
