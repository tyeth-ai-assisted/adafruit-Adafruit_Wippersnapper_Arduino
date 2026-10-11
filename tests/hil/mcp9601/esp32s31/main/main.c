/*
 * MCP9601 failure/recovery experiment, ESP32-S31 (ESP-IDF 6.x+).
 * Experimental fixture: do not reuse mux-bus GPIOs. No automatic probes at boot.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SCL CONFIG_MCP_HIL_SCL_GPIO
#define SDA CONFIG_MCP_HIL_SDA_GPIO
#define OBSCL CONFIG_MCP_HIL_SNIFF_SCL_GPIO
#define OBSDA CONFIG_MCP_HIL_SNIFF_SDA_GPIO
#define PORT CONFIG_MCP_HIL_I2C_PORT
#define ADDR CONFIG_MCP_HIL_MCP_ADDRESS
#define SPEED CONFIG_MCP_HIL_SPEED_HZ
#define TAG "mcp-hil"
static i2c_master_bus_handle_t bus;
static bool active;
static const int pins[] = {SCL,SDA,OBSCL,OBSDA};
static bool pins_valid(void) {
    for (int i=0;i<4;i++) {
        if (pins[i]<0 || !GPIO_IS_VALID_GPIO(pins[i])) return false;
        for (int j=0;j<i;j++) if (pins[j]==pins[i]) return false;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(SCL) || !GPIO_IS_VALID_OUTPUT_GPIO(SDA)) return false;
    if (SCL==0 || SDA==0 || OBSCL==0 || OBSDA==0) ESP_LOGW(TAG,"Check boot strapping pin GPIO0");
    return true;
}
static esp_err_t init_bus(void) {
    i2c_master_bus_config_t c = {
        .i2c_port=PORT, .sda_io_num=SDA, .scl_io_num=SCL,
        .clk_source=I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt=7,
        .flags.enable_internal_pullup=true
    };
    esp_err_t e=i2c_new_master_bus(&c,&bus);
    active=(e==ESP_OK);
    return e;
}
static esp_err_t release_bus(void) {
    if (!active) return ESP_OK;
    esp_err_t e=i2c_del_master_bus(bus);
    if (e==ESP_OK) {active=false;bus=NULL;}
    return e;
}
static esp_err_t probe(int addr) {
    return i2c_master_probe(bus,addr,100);
}
static esp_err_t read_id(void) {
    i2c_device_config_t cfg = {.dev_addr_length=I2C_ADDR_BIT_LEN_7,
        .device_address=ADDR, .scl_speed_hz=SPEED};
    i2c_master_dev_handle_t dev=NULL;
    esp_err_t e=i2c_master_bus_add_device(bus,&cfg,&dev);
    if(e!=ESP_OK) return e;
    uint8_t reg=0x20, data[2]={0,0};
    e=i2c_master_transmit_receive(dev,&reg,1,data,2,100);
    esp_err_t cleanup=i2c_master_bus_rm_device(dev);
    if(e==ESP_OK) printf("ID=0x%02X%02X expected high byte 0x41 (%s)\n",
       data[0],data[1],data[0]==0x41?"match":"MISMATCH");
    else printf("ID read failed: %s\n",esp_err_to_name(e));
    if(cleanup!=ESP_OK) ESP_LOGE(TAG,"remove device: %s",esp_err_to_name(cleanup));
    return e;
}
static void levels(void) {
    printf("TEST_BUS_SCL=%d SDA=%d OBS_SCL=%d OBS_SDA=%d\n",
       gpio_get_level(SCL),gpio_get_level(SDA),
       gpio_get_level(OBSCL),gpio_get_level(OBSDA));
}
static void idle_us(unsigned microseconds) { esp_rom_delay_us(microseconds); }
static void pull_low(int pin) {
    gpio_set_level(pin,0); // preset output latch before enabling output mode
    gpio_set_direction(pin,GPIO_MODE_INPUT_OUTPUT_OD);
}
static void release_pin(int pin) {
    gpio_set_level(pin,1); // avoid a spurious falling edge on mode transition
    gpio_set_direction(pin,GPIO_MODE_INPUT_OUTPUT_OD);
}
static bool wait_high(int pin) {
    for(int i=0;i<100;i++) {if(gpio_get_level(pin)) return true;idle_us(10);}
    return false;
}
/* Optional physical loopback preflight. SDA changes only while SCL is LOW,
 * avoiding an unaddressed START or STOP. One SCL excursion is nevertheless
 * generated; run only after a known-good, idle baseline, never as recovery.
 */
static void wiring_test(void) {
    esp_err_t e=release_bus();
    if(e!=ESP_OK) {
        printf("wiring: release failed: %s\n",esp_err_to_name(e));
        return;
    }
    release_pin(SCL);
    release_pin(SDA);
    idle_us(40);
    bool idle=wait_high(SCL) && wait_high(SDA)
        && gpio_get_level(OBSCL)==1 && gpio_get_level(OBSDA)==1;
    pull_low(SCL);
    idle_us(40);
    bool scl_low=gpio_get_level(OBSCL)==0 && gpio_get_level(OBSDA)==1;
    pull_low(SDA);
    idle_us(40);
    bool sda_low=gpio_get_level(OBSCL)==0 && gpio_get_level(OBSDA)==0;
    release_pin(SDA);
    idle_us(40);
    bool sda_released=gpio_get_level(OBSCL)==0 && gpio_get_level(OBSDA)==1;
    release_pin(SCL);
    idle_us(40);
    bool released=wait_high(SCL) && gpio_get_level(OBSCL)==1
        && gpio_get_level(OBSDA)==1;
    e=init_bus();
    printf("wiring: idle=%s scl_follows=%s sda_follows=%s "
           "sda_releases=%s both_release=%s reinit=%s\n",
           idle?"PASS":"FAIL", scl_low?"PASS":"FAIL",sda_low?"PASS":"FAIL",
           sda_released?"PASS":"FAIL",released?"PASS":"FAIL",esp_err_to_name(e));
}
static esp_err_t forced_stop_and_clocks(int count) {
    esp_err_t e=release_bus();
    if(e!=ESP_OK) return e;
    // Use open-drain transitions, never drive a HIGH level.
    release_pin(SDA);release_pin(SCL);idle_us(20);
    if (!wait_high(SCL)) { e=ESP_ERR_TIMEOUT;goto done; }
    for (int i=0;i<count;i++) {
        pull_low(SCL);idle_us(20);release_pin(SCL);
        if(!wait_high(SCL)){e=ESP_ERR_TIMEOUT;goto done;}
        idle_us(20);
    }
    // Deliberate START-like SDA fall with SCL high, followed by STOP release.
    pull_low(SDA);idle_us(20);release_pin(SCL);
    if(!wait_high(SCL)){e=ESP_ERR_TIMEOUT;goto done;}
    idle_us(20);release_pin(SDA);idle_us(20);
done:
    release_pin(SDA);release_pin(SCL);idle_us(20);
    esp_err_t r=init_bus();
    if (e==ESP_OK) e=r;
    printf("manual %d clocks + STOP: %s\n",count,esp_err_to_name(e));
    return e;
}
#define NSAMPLES 160
typedef struct {
    rmt_channel_handle_t ch;
    rmt_symbol_word_t samples[NSAMPLES];
    volatile bool completed;
    size_t count;
    int gpio;
    const char *label;
} capture_t;
static bool IRAM_ATTR on_rx(rmt_channel_handle_t ch,const rmt_rx_done_event_data_t *event,void *arg) {
    capture_t *c=arg;
    c->completed=true;
    c->count=event->num_symbols;
    return false;
}
static esp_err_t start_capture(capture_t *c,int pin,const char *label) {
    memset(c,0,sizeof(*c));c->gpio=pin;c->label=label;
    rmt_rx_channel_config_t cfg={.gpio_num=pin,.clk_src=RMT_CLK_SRC_DEFAULT,
        .resolution_hz=1000000,.mem_block_symbols=64};
    esp_err_t e=rmt_new_rx_channel(&cfg,&c->ch);
    if(e!=ESP_OK)return e;
    rmt_rx_event_callbacks_t cb={.on_recv_done=on_rx};
    e=rmt_rx_register_event_callbacks(c->ch,&cb,c);
    if(e!=ESP_OK)return e;
    e=rmt_enable(c->ch);
    if(e!=ESP_OK)return e;
    rmt_receive_config_t rx={.signal_range_min_ns=1000,.signal_range_max_ns=3000000};
    return rmt_receive(c->ch,c->samples,sizeof(c->samples),&rx);
}
static void finish_capture(capture_t *c) {
    for(int i=0;i<300 && !c->completed;i++)vTaskDelay(pdMS_TO_TICKS(1));
    printf("%s completed=%d count=%u (independent RMT time origin)\n",c->label,
       c->completed,(unsigned)c->count);
    size_t n=c->count < NSAMPLES ? c->count:NSAMPLES;
    for(size_t i=0;i<n;i++) {
        rmt_symbol_word_t *s=&c->samples[i];
        printf("%s[%u] %d:%uus %d:%uus\n",c->label,(unsigned)i,
          s->level0,(unsigned)s->duration0,s->level1,(unsigned)s->duration1);
    }
    if(c->ch){rmt_disable(c->ch);rmt_del_channel(c->ch);}
}
static void captured_probe(bool at50) {
    capture_t scl,sda;
    esp_err_t e=start_capture(&scl,OBSCL,"SCL");
    if(e!=ESP_OK){printf("RMT SCL failed %s\n",esp_err_to_name(e));return;}
    e=start_capture(&sda,OBSDA,"SDA");
    if(e!=ESP_OK){printf("RMT SDA failed %s\n",esp_err_to_name(e));finish_capture(&scl);return;}
    vTaskDelay(pdMS_TO_TICKS(2));
    if(at50) printf("probe 0x50 -> %s\n",esp_err_to_name(probe(0x50)));
    else read_id();
    finish_capture(&scl);finish_capture(&sda);
}
static void help(void) {
    puts("help | id | scan | probe50 | probemcp | captureid | capture50 | levels | wiring");
    puts("wait10 | reinit | stop | pulse1 | pulse9 | pulse18");
}
void app_main(void) {
    if(!pins_valid()) {
        ESP_LOGE(TAG,"Invalid/unconfigured GPIOs. Set distinct TEST and OBSERVE pins in menuconfig; never reuse analogue-mux pins.");
        return;
    }
    gpio_set_direction(OBSCL,GPIO_MODE_INPUT);
    gpio_set_direction(OBSDA,GPIO_MODE_INPUT);
    esp_err_t e=init_bus();
    if(e!=ESP_OK){ESP_LOGE(TAG,"I2C init: %s",esp_err_to_name(e));return;}
    printf("MCP9601 HIL READY, SCL=%d SDA=%d sniff=%d/%d addr=0x%02x\n",
       SCL,SDA,OBSCL,OBSDA,ADDR);
    puts("No automatic I2C transactions. Type help.");
    char cmd[32];
    while (fgets(cmd,sizeof(cmd),stdin)) {
        cmd[strcspn(cmd,"\r\n")]=0;
        if(!strcmp(cmd,"help"))help();
        else if(!strcmp(cmd,"id"))read_id();
        else if(!strcmp(cmd,"levels"))levels();
        else if(!strcmp(cmd,"wiring"))wiring_test();
        else if(!strcmp(cmd,"scan")) {
            for(int a=0x08;a<=0x77;a++) {
                esp_err_t p=probe(a);
                if(p==ESP_OK) printf("ACK 0x%02X\n",a);
                else if(p!=ESP_ERR_NOT_FOUND) printf("0x%02X: %s\n",a,esp_err_to_name(p));
            }
            puts("full scan complete (MCP address INCLUDED)");
        }
        else if(!strcmp(cmd,"probe50") || !strcmp(cmd,"probemcp")) {
            int a=!strcmp(cmd,"probe50")?0x50:ADDR;
            printf("probe 0x%02X: %s\n",a,esp_err_to_name(probe(a)));
        }
        else if(!strcmp(cmd,"captureid"))captured_probe(false);
        else if(!strcmp(cmd,"capture50"))captured_probe(true);
        else if(!strcmp(cmd,"wait10")) {vTaskDelay(pdMS_TO_TICKS(10000));puts("wait complete");}
        else if(!strcmp(cmd,"reinit")) {
            e=release_bus();
            if(e==ESP_OK)e=init_bus();
            printf("reinit: %s\n",esp_err_to_name(e));
        }
        else if(!strcmp(cmd,"stop"))forced_stop_and_clocks(0);
        else if(!strcmp(cmd,"pulse1"))forced_stop_and_clocks(1);
        else if(!strcmp(cmd,"pulse9"))forced_stop_and_clocks(9);
        else if(!strcmp(cmd,"pulse18"))forced_stop_and_clocks(18);
        else if(cmd[0])printf("unknown command: %s\n",cmd);
    }
}
