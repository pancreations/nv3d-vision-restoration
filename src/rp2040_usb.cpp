// SPDX-License-Identifier: GPL-3.0-or-later
#include "rp2040_usb.h"
#include "rp2040_usb_packet.h"
#include <Windows.h>
#include <stdexcept>
#include <sstream>
#include <libusb.h>
namespace vision::rp2040 {
double hostMicroseconds(){LARGE_INTEGER t,f;QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);return double(t.QuadPart)*1e6/double(f.QuadPart);}
namespace {
constexpr uint16_t vid=0xcafe,pid=0x3d02;
struct Context {libusb_context* p=nullptr;Context(){if(libusb_init(&p)<0)throw std::runtime_error("libusb init failed");}~Context(){libusb_exit(p);}};
class UsbLink final : public Link {
    Context context_;libusb_device_handle* handle_=nullptr;bool claimed_=false;std::string serial_,firmware_;
public:
    UsbLink(uint8_t bus,uint8_t address) {
        libusb_device** list=nullptr;auto n=libusb_get_device_list(context_.p,&list);
        for(ptrdiff_t i=0;i<n;++i) {
            libusb_device_descriptor d{};libusb_get_device_descriptor(list[i],&d);
            if(d.idVendor==vid&&d.idProduct==pid&&libusb_get_bus_number(list[i])==bus&&libusb_get_device_address(list[i])==address) {
                if(libusb_open(list[i],&handle_)<0)handle_=nullptr;break;
            }
        }
        if(list)libusb_free_device_list(list,1);
        if(!handle_)throw std::runtime_error("Cannot open RP2040. Flash VisionEmitter.uf2 and inspect its WinUSB binding; do not select the NVIDIA firmware loader.");
        try {
            libusb_device_descriptor d{};libusb_get_device_descriptor(libusb_get_device(handle_),&d);
            unsigned char product[128]{};const auto len=libusb_get_string_descriptor_ascii(handle_,d.iProduct,product,sizeof(product));
            if(len<0||std::string(reinterpret_cast<char*>(product),size_t(len))!="Vision RP2040 Emitter v1"||d.bcdDevice!=0x0200)
                throw std::runtime_error("USB product/version does not match our RP2040 firmware; no commands sent");
            unsigned char serial[128]{};const auto serialLength=libusb_get_string_descriptor_ascii(handle_,d.iSerialNumber,serial,sizeof(serial));
            if(serialLength<=0)throw std::runtime_error("RP2040 serial identity is missing");
            serial_.assign(reinterpret_cast<char*>(serial),size_t(serialLength));
            unsigned char firmware[128]{};const auto firmwareLength=libusb_get_string_descriptor_ascii(handle_,5,firmware,sizeof(firmware));
            if(firmwareLength<=0)throw std::runtime_error("RP2040 firmware build identity missing");
            firmware_.assign(reinterpret_cast<char*>(firmware),size_t(firmwareLength));
            if(!firmware_.starts_with("VRP1/0.2.0/")&&!firmware_.starts_with("VRP1/0.3.0/")&&!firmware_.starts_with("VRP1/0.4.0/"))throw std::runtime_error("Unsupported RP2040 firmware version");
            libusb_config_descriptor* c=nullptr;
            if(libusb_get_active_config_descriptor(libusb_get_device(handle_),&c)<0||!c)throw std::runtime_error("Cannot inspect RP2040 endpoints");
            bool valid=false;
            if(c->bNumInterfaces==1&&c->interface[0].num_altsetting==1) {
                const auto& a=c->interface[0].altsetting[0];bool in=false,out=false;
                for(uint8_t i=0;i<a.bNumEndpoints;++i){const auto& e=a.endpoint[i];if((e.bmAttributes&3)==2&&e.wMaxPacketSize==64){in|=e.bEndpointAddress==0x81;out|=e.bEndpointAddress==1;}}
                valid=a.bInterfaceNumber==0&&a.bAlternateSetting==0&&a.bInterfaceClass==0xff&&in&&out;
            }
            libusb_free_config_descriptor(c);if(!valid)throw std::runtime_error("RP2040 USB endpoint layout mismatch");
            if(libusb_claim_interface(handle_,0)<0)throw std::runtime_error("RP2040 interface is busy or WinUSB binding is unavailable");
            claimed_=true;
            // A previously cancelled host exchange may have left its reply
            // queued on the board. Drain only IN, before this client's first
            // request, so an old reply cannot be mistaken for a new Status.
            const double drainDeadline=hostMicroseconds()+20000;
            for(;;) {
                if(hostMicroseconds()>=drainDeadline)throw std::runtime_error("RP2040 USB IN did not become idle on connect");
                Packet stale{};int received=0;
                const int result=libusb_bulk_transfer(handle_,0x81,stale.data(),int(stale.size()),&received,2);
                if(result==LIBUSB_ERROR_TIMEOUT&&received==0)break;
                if(result<0&&result!=LIBUSB_ERROR_TIMEOUT)throw std::runtime_error(std::string("RP2040 USB IN drain failed: ")+libusb_error_name(result));
            }
        }catch(...){libusb_close(handle_);handle_=nullptr;throw;}
    }
    ~UsbLink(){if(handle_){if(claimed_)libusb_release_interface(handle_,0);libusb_close(handle_);}}
    std::string identity()const override{return "rp2040:"+serial_;}
    std::string firmwareVersion()const override{return firmware_;}
    Packet exchange(const Packet& request,std::stop_token stop)override {
        if(stop.stop_requested())throw std::runtime_error("RP2040 transfer cancelled");
        int count=0;auto outgoing=request;
        int r=libusb_bulk_transfer(handle_,1,outgoing.data(),64,&count,20);
        if(r<0||count!=64)throw std::runtime_error(std::string("RP2040 USB OUT failed: ")+libusb_error_name(r)+" ("+std::to_string(count)+"/64 bytes)");
        if(stop.stop_requested())throw std::runtime_error("RP2040 transfer cancelled; device watchdog will stop output");
        return receiveUsbReply([&](Packet& incoming,unsigned timeout){
            int received=0;
            const int result=libusb_bulk_transfer(handle_,0x81,incoming.data(),int(incoming.size()),&received,timeout);
            if(result<0)throw std::runtime_error(std::string("RP2040 USB IN failed: ")+libusb_error_name(result));
            return size_t(received);
        },hostMicroseconds,stop);
    }
};
}
std::vector<UsbAddress> discoverUsb() {
    Context context;libusb_device** list=nullptr;auto n=libusb_get_device_list(context.p,&list);std::vector<UsbAddress> found;
    if(n<0)throw std::runtime_error("RP2040 USB enumeration failed");
    for(ptrdiff_t i=0;i<n;++i){libusb_device_descriptor d{};if(libusb_get_device_descriptor(list[i],&d)==0&&d.idVendor==vid&&d.idProduct==pid)
        found.push_back({libusb_get_bus_number(list[i]),libusb_get_device_address(list[i]),"RP2040 candidate cafe:3d02 (identity checked on connect)"});}
    libusb_free_device_list(list,1);return found;
}
std::unique_ptr<Link> openUsb(uint8_t bus,uint8_t address){return std::make_unique<UsbLink>(bus,address);}
}
