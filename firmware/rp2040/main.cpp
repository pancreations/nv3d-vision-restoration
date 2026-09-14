// SPDX-License-Identifier: GPL-3.0-or-later
#include "rp2040_protocol.h"
#include "rp2040_wave.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/rand.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "tusb.h"
#include <optional>
using namespace vision::rp2040;
namespace {
constexpr uint pin=2;
critical_section_t gate;
std::optional<DeviceEndpoint> endpoint;
PIO ir=pio0;uint sm=0,offset=0;uint64_t busyUntil=0;
volatile uint64_t core1Alive=0;
// pull block; out pins,1; out x,31; jmp x--,loop. Short tokens fit completely
// in the joined FIFO; DMA is unnecessary. Completion includes FIFO drain.
uint16_t instructions[4];
void idle() {
    pio_sm_set_enabled(ir,sm,false);pio_sm_clear_fifos(ir,sm);
    pio_sm_exec(ir,sm,pio_encode_set(pio_pins,0));busyUntil=0;
}
void initIr() {
    gpio_init(pin);gpio_set_dir(pin,GPIO_OUT);gpio_put(pin,0);
    instructions[0]=pio_encode_pull(false,true);instructions[1]=pio_encode_out(pio_pins,1);
    instructions[2]=pio_encode_out(pio_x,31);instructions[3]=pio_encode_jmp_x_dec(3);
    const pio_program program={instructions,4,-1,0};
    sm=pio_claim_unused_sm(ir,true);offset=pio_add_program(ir,&program);
    auto c=pio_get_default_sm_config();sm_config_set_wrap(&c,offset,offset+3);
    sm_config_set_out_pins(&c,pin,1);sm_config_set_set_pins(&c,pin,1);
    sm_config_set_out_shift(&c,false,false,32);sm_config_set_fifo_join(&c,PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c,float(clock_get_hz(clk_sys))/1000000.0f);
    pio_gpio_init(ir,pin);pio_sm_set_consecutive_pindirs(ir,sm,pin,1,true);
    pio_sm_init(ir,sm,offset,&c);idle();
}
bool fire(const Action& a,uint64_t now) {
    if(now<busyUntil)return false;
    auto wave=makeWave(a.kind,a.eye);if(!wave.count)return false;
    idle();pio_sm_restart(ir,sm);pio_sm_clkdiv_restart(ir,sm);
    pio_sm_exec(ir,sm,pio_encode_jmp(offset));
    for(size_t i=0;i<wave.count;++i)pio_sm_put(ir,sm,wave.words[i]);
    busyUntil=now+wave.durationUs+20;pio_sm_set_enabled(ir,sm,true);return true;
}
void timingCore() {
    while(true) {
        critical_section_enter_blocking(&gate);
        const auto now=time_us_64();core1Alive=now;
        auto step=endpoint->advance(now);
        if(step.forceIdle)idle();
        for(size_t i=0;i<step.count;++i)if(!fire(step.actions[i],now)){endpoint->disconnect();idle();break;}
        critical_section_exit(&gate);
        tight_loop_contents();
    }
}
void disconnectDevice() {
    critical_section_enter_blocking(&gate);endpoint->disconnect();idle();critical_section_exit(&gate);
}
}
extern "C" void tud_umount_cb(){disconnectDevice();}
extern "C" void tud_suspend_cb(bool){disconnectDevice();}
int main() {
    critical_section_init(&gate);endpoint.emplace(get_rand_64()|1u,false);initIr();
    multicore_launch_core1(timingCore);tusb_init();watchdog_enable(500,true);
    Packet incoming{};size_t have=0;uint64_t partialSince=0;
    std::optional<Packet> outgoing;
    while(true) {
        tud_task();const auto now=time_us_64();
        critical_section_enter_blocking(&gate);const auto alive=core1Alive;critical_section_exit(&gate);
        if(now>=alive&&now-alive<100000)watchdog_update();
        if(!tud_mounted()) {have=0;outgoing.reset();continue;}
        if(outgoing&&tud_vendor_write_available()>=64) {
            // Device timestamp marks enqueue into TinyUSB, not the physical bus.
            Message reply;if(decode(*outgoing,reply)==Result::Ok&&reply.payload[1]==uint8_t(Opcode::Clock)) {
                put64(std::span(reply.payload).subspan(16),time_us_64());outgoing=encode(reply);
            }
            if(tud_vendor_write(outgoing->data(),64)==64){tud_vendor_write_flush();outgoing.reset();}
        }
        if(have&&now-partialSince>20000){have=0;tud_vendor_read_flush();disconnectDevice();}
        if(!outgoing&&tud_vendor_available()) {
            if(!have)partialSince=now;
            have+=tud_vendor_read(incoming.data()+have,uint32_t(64-have));
            if(have==64) {
                critical_section_enter_blocking(&gate);
                const auto stamp=time_us_64();auto exchange=endpoint->receive(incoming,stamp,stamp);
                if(exchange.forceIdle)idle();
                outgoing=exchange.reply;
                critical_section_exit(&gate);have=0;
            }
        }
    }
}
