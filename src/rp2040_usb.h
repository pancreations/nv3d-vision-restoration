// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "rp2040_host.h"
#include <memory>
#include <vector>
namespace vision::rp2040 {
struct UsbAddress { uint8_t bus=0,address=0;std::string label; };
std::vector<UsbAddress> discoverUsb();
std::unique_ptr<Link> openUsb(uint8_t bus,uint8_t address);
double hostMicroseconds();
}
