// SPDX-License-Identifier: GPL-3.0-or-later
#include "tusb.h"
#include "pico/unique_id.h"
#include <array>
#include <cstring>

// Private prototype identity, NOT an assigned production VID/PID. A matching
// product string and VRP1 handshake are required by the host before commands.
static const tusb_desc_device_t device={18,TUSB_DESC_DEVICE,0x0210,0,0,0,64,0xcafe,0x3d02,0x0200,1,2,3,1};
static const uint8_t configuration[]={
    TUD_CONFIG_DESCRIPTOR(1,1,0,TUD_CONFIG_DESC_LEN+TUD_VENDOR_DESC_LEN,0,500),
    TUD_VENDOR_DESCRIPTOR(0,4,0x01,0x81,64)
};
static constexpr uint8_t osRequest=0x21;
static constexpr uint16_t osLength=178;
static const uint8_t bos[]={
    TUD_BOS_DESCRIPTOR(TUD_BOS_DESC_LEN+TUD_BOS_MICROSOFT_OS_DESC_LEN,1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(osLength,osRequest)
};
static std::array<uint8_t,osLength> makeOs() {
    std::array<uint8_t,osLength> b{};size_t n=0;
    auto u16=[&](uint16_t v){b[n++]=uint8_t(v);b[n++]=uint8_t(v>>8);};
    auto u32=[&](uint32_t v){for(int i=0;i<4;++i)b[n++]=uint8_t(v>>(8*i));};
    auto text=[&](const char* s){do{u16(uint8_t(*s));}while(*s++);};
    u16(10);u16(0);u32(0x06030000);u16(osLength);
    u16(8);u16(1);b[n++]=0;b[n++]=0;u16(osLength-10);
    u16(8);u16(2);b[n++]=0;b[n++]=0;u16(osLength-18);
    u16(20);u16(3);for(char c:std::array<char,8>{'W','I','N','U','S','B',0,0})b[n++]=uint8_t(c);n+=8;
    u16(132);u16(4);u16(7);u16(42);text("DeviceInterfaceGUIDs");u16(80);
    text("{BA3D0147-E2F0-4C81-9C95-A113895B3204}");u16(0);
    return b;
}
static const auto os=makeOs();
extern "C" {
const uint8_t* tud_descriptor_device_cb(){return reinterpret_cast<const uint8_t*>(&device);}
const uint8_t* tud_descriptor_configuration_cb(uint8_t){return configuration;}
const uint8_t* tud_descriptor_bos_cb(){return bos;}
const uint16_t* tud_descriptor_string_cb(uint8_t index,uint16_t) {
    static uint16_t b[64];static char serial[2*PICO_UNIQUE_BOARD_ID_SIZE_BYTES+1];
    if(index==0){b[0]=0x0304;b[1]=0x0409;return b;}
    pico_get_unique_board_id_string(serial,sizeof(serial));
    const char* strings[]={"","Vision Restoration Lab","Vision RP2040 Emitter v1",serial,"VRP1 scheduled IR",VISION_FIRMWARE_ID};
    if(index>=6)return nullptr;
    const auto len=std::strlen(strings[index]);for(size_t i=0;i<len;++i)b[i+1]=uint8_t(strings[index][i]);
    b[0]=uint16_t((TUSB_DESC_STRING<<8)|(2+2*len));return b;
}
bool tud_vendor_control_xfer_cb(uint8_t port,uint8_t stage,const tusb_control_request_t* r) {
    if(stage!=CONTROL_STAGE_SETUP)return true;
    if(r->bmRequestType==0xc0&&r->bRequest==osRequest&&r->wIndex==7)
        return tud_control_xfer(port,r,const_cast<uint8_t*>(os.data()),uint16_t(os.size()));
    return false;
}
}
