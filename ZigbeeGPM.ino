/* **** **** **** **** ****
Zogbee Grid Power Monitor
Arduino Sketch

Waiting for the first days results - VERY ALPHA! Correct, not even beta yet!
TODO: Remove serial printline when no longer connected to laptop for testing
TODO: Make SLEEP TIME a user definable var
TODO: Expost some kind of 'gradually increase sleep time' after being installed
TODO: Make default sleep time just a few seconds at first
TODO: Consider bundling reports into same chunks as power company / legislation / bill, eg 5/15/30 min intervals?
TODO: Maybe give sensor memory between boots
TODO: … dont let this dev chew much time!
**** **** **** **** **** */


#ifndef ZIGBEE_MODE_ED
#error "Zigbee end device mode is not selected in Tools->Zigbee mode, also check partition is of type zigbee SOMETHING"
#endif

#include "Zigbee.h"
#include "driver/pulse_cnt.h"
#include "driver/gpio.h"
#include "esp_sleep.h"

/* Pin and meter configuration */
#define PULSE_INPUT_PIN GPIO_NUM_4
#define PULSES_PER_KWH 1000

/* 
Zigbee configuration 
NB
Changing endpoint sensor number can cause unstable results
*/
#define POWER_SENSOR_ENDPOINT_NUMBER 67
#define uS_TO_S_FACTOR 1000000ULL
#define TIME_TO_SLEEP 3600  // Sleep for 1 hour

/* PCNT (Pulse Counter) setup */
static pcnt_unit_handle_t pcnt_unit = NULL;

/* RTC memory - survives light sleep */
static RTC_DATA_ATTR int rtc_pulse_count = 0;
static RTC_DATA_ATTR float rtc_total_kwh = 0.0;

/* Create Zigbee electrical measurement device */
ZigbeeElectricalMeasurement zbPowerSensor = ZigbeeElectricalMeasurement(POWER_SENSOR_ENDPOINT_NUMBER);

/* **** **** **** **** **** PCNT Configuration **** **** **** **** **** */
void init_pcnt(void) {
    Serial.println("Initializing PCNT on GPIO4");
    
    // Configure GPIO with internal pullup (matching ESPHome config)
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << PULSE_INPUT_PIN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;  // INPUT_PULLUP like ESPHome (use internal resister)
    gpio_config(&io_conf);
    
    // PCNT unit configuration
    pcnt_unit_config_t unit_config = {
        .low_limit = -32768,
        .high_limit = 32767,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &pcnt_unit));
    
    // PCNT channel configuration
    pcnt_chan_config_t chan_config = {
        .edge_gpio_num = PULSE_INPUT_PIN,
        .level_gpio_num = -1,
    };
    pcnt_channel_handle_t pcnt_chan = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(pcnt_unit, &chan_config, &pcnt_chan));
    
    // Count on falling edge (inverted signal with pullup = active low)
    // ESPHome "inverted: true" means falling edge is the active edge
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan, 
        PCNT_CHANNEL_EDGE_ACTION_DECREASE, 
        PCNT_CHANNEL_EDGE_ACTION_HOLD));
    
    // Enable glitch filter (1us debounce)
    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = 1000,
    };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(pcnt_unit, &filter_config));
    
    // Enable and start counting
    ESP_ERROR_CHECK(pcnt_unit_enable(pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(pcnt_unit));
    
    Serial.println("PCNT initialized and started");
}

int read_pulse_count(void) {
    int count = 0;
    ESP_ERROR_CHECK(pcnt_unit_get_count(pcnt_unit, &count));
    return abs(count);
}

/* **** **** **** **** **** Measure and Report **** **** **** **** **** */
void measureAndReport() {
    // Read current pulse count
    int current_pulses = read_pulse_count();
    
    // Calculate energy since last report
    int pulse_delta = current_pulses - rtc_pulse_count;
    float kwh_delta = (float)pulse_delta / PULSES_PER_KWH;
    
    // Update running total
    rtc_total_kwh += kwh_delta;
    rtc_pulse_count = current_pulses;
    
    // Calculate average power (W) over the hour
    float avg_power_w = kwh_delta * 1000.0;
    
    // Read internal temperature
    float internal_temp = temperatureRead();
    
    // Update DC power measurement
    // Power in milliwatts (API expects int16_t in mW)
    int16_t power_mw = (int16_t)(avg_power_w * 1000.0);
    zbPowerSensor.setDCMeasurement(ZIGBEE_DC_MEASUREMENT_TYPE_POWER, power_mw);
    
    // Use voltage field to represent total energy
    // Map kWh to millivolts (so we can send the value)
    // 1 kWh = 1000 mV in our mapping
    int16_t energy_mv = (int16_t)(rtc_total_kwh * 1000.0);
    zbPowerSensor.setDCMeasurement(ZIGBEE_DC_MEASUREMENT_TYPE_VOLTAGE, energy_mv);
    
    Serial.printf("Hourly Report:\n");
    Serial.printf("  Pulses this hour: %d\n", pulse_delta);
    Serial.printf("  Energy this hour: %.3f kWh\n", kwh_delta);
    Serial.printf("  Total energy: %.3f kWh\n", rtc_total_kwh);
    Serial.printf("  Avg power: %.1f W (%.1f mW)\n", avg_power_w, power_mw / 1.0);
    Serial.printf("  Internal temp: %.1f°C\n", internal_temp);
    
    // Report values via Zigbee
    zbPowerSensor.reportDC(ZIGBEE_DC_MEASUREMENT_TYPE_POWER);
    zbPowerSensor.reportDC(ZIGBEE_DC_MEASUREMENT_TYPE_VOLTAGE);
    
    Serial.println("Data reported to Zigbee coordinator");
}

/***** **** **** **** **** Arduino Setup **** **** **** **** **** */
void setup() {
    Serial.begin(115200);
    
    // CRITICAL: Give time to upload new firmware before sleeping!
    Serial.println("Power Meter Zigbee Starting...");
    Serial.println("Waiting 10 seconds - press BOOT button now to prevent sleep for 60s");
    for(int i = 10; i > 0; i--) {
        Serial.printf("%d...", i);
        delay(1000);
        if(digitalRead(BOOT_PIN) == LOW) {
            Serial.println("\nBOOT pressed - staying awake for 60 seconds!");
            delay(60000);
            Serial.println("Continuing with normal startup...");
            break;
        }
    }
    Serial.println("\nStarting...");
    
    // Configure wake up timer
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
    
    // Initialize PCNT
    init_pcnt();
    
    // Configure Zigbee electrical measurement sensor - I made this - I am Unapproved - Asher Lee!
    zbPowerSensor.setManufacturerAndModel("Unapproved", "PowerMeter");
    
    // Add DC measurements for power and voltage (repurposing voltage for energy)
    zbPowerSensor.addDCMeasurement(ZIGBEE_DC_MEASUREMENT_TYPE_POWER);
    zbPowerSensor.addDCMeasurement(ZIGBEE_DC_MEASUREMENT_TYPE_VOLTAGE);
    
    // Set min/max values
    // Power: 0-32767 mW (0-32.767 W) - int16_t max
    zbPowerSensor.setDCMinMaxValue(ZIGBEE_DC_MEASUREMENT_TYPE_POWER, 0, 32767);
    // Voltage (energy): 0-32767 mV (0-32.767 kWh in our mapping)
    zbPowerSensor.setDCMinMaxValue(ZIGBEE_DC_MEASUREMENT_TYPE_VOLTAGE, 0, 32767);
    
    // Set multiplier/divisor
    // Power: 1/1000 = 0.001W (1 unit = 1mW)
    zbPowerSensor.setDCMultiplierDivisor(ZIGBEE_DC_MEASUREMENT_TYPE_POWER, 1, 1000);
    // Voltage: 1/1000 = 0.001V (1 unit = 1mV, represents 0.001 kWh in our mapping)
    zbPowerSensor.setDCMultiplierDivisor(ZIGBEE_DC_MEASUREMENT_TYPE_VOLTAGE, 1, 1000);
    
    // Set power source to battery
    zbPowerSensor.setPowerSource(ZB_POWER_SOURCE_BATTERY, 100);
    
    // Add endpoint to Zigbee
    Zigbee.addEndpoint(&zbPowerSensor);
    
    // Create custom config for sleepy end device
    esp_zb_cfg_t zigbeeConfig = ZIGBEE_DEFAULT_ED_CONFIG();
    // Increase keep-alive for better stability
    zigbeeConfig.nwk_cfg.zed_cfg.keep_alive = 3000;  // 3 seconds
    
    // Start Zigbee with custom config
    Serial.println("Starting Zigbee...");
    if (!Zigbee.begin(&zigbeeConfig, false)) {
        Serial.println("Zigbee failed to start!");
        Serial.println("Rebooting...");
        ESP.restart();
    }
    
    Serial.println("Connecting to Zigbee network...");
    while (!Zigbee.connected()) {
        Serial.print(".");
        delay(100);
    }
    Serial.println("\nConnected to Zigbee network!");
    
    // Stay awake for interview - Z2M needs time to configure the device
    Serial.println("Staying awake for 60 seconds to allow coordinator to interview...");
    delay(60000);
    
    // Optional: Set reporting intervals (might be overridden by Z2M during interview)
    // Report every hour (3600s) or if value changes
    zbPowerSensor.setDCReporting(ZIGBEE_DC_MEASUREMENT_TYPE_POWER, 0, 3600, 100);
    zbPowerSensor.setDCReporting(ZIGBEE_DC_MEASUREMENT_TYPE_VOLTAGE, 0, 3600, 10);
    
    // Do initial measurement and report
    measureAndReport();
    
    // Give time for Zigbee to transmit
    delay(5000);
    
    Serial.println("Entering light sleep for 1 hour...");
    Serial.println("PCNT will continue counting during sleep");
}

/***** **** **** **** **** Arduino Loop **** **** **** **** **** */
void loop() {
    // Enter light sleep - PCNT continues counting
    esp_light_sleep_start();
    
    // Woke up after 1 hour
    Serial.println("\nWoke from sleep!");
    
    // Re-establish Zigbee connection if needed
    if (!Zigbee.connected()) {
        Serial.println("Reconnecting to Zigbee...");
        while (!Zigbee.connected()) {
            delay(100);
        }
        Serial.println("Reconnected!");
    }
    
    // Measure and report
    measureAndReport();
    
    // Give time for Zigbee transmission, milliseconds - 5 seconds = 5000
    delay(5000);
    
    Serial.println("Going back to sleep...");
}
