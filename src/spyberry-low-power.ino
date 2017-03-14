/*
 ******************************************************************************
 *  Copyright (c) 2016 Particle Industries, Inc.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************
 */

#include "Particle.h"
#include "CellularHelper.h"
#include "HttpClient.h"

// Use either MANUAL or SEMI_AUTOMATIC mode to keep the cellular modem off initially on boot
SYSTEM_MODE(MANUAL);
// SYSTEM_MODE(SEMI_AUTOMATIC);

// Time between sensor readings in seconds
#define SENSOR_READING_INTERVAL 60*2
// We will publish the entire array when it's full
#define SENSOR_RECORD_ARRAY_SIZE 1
typedef struct
{
    time_t    timestamp_unix;
    int       cell_mcc;
    int       cell_mnc;
    int       cell_lac;
    int       cell_ci;
    float     state_of_charge;
    // 24 bytes (the cell params could be uint_16 if needed)
} SensorRecordDef;
// If we lose power completely, expect these to reinitialize.
retained SensorRecordDef sensor_record[SENSOR_RECORD_ARRAY_SIZE] = {0};
//retained float sensor_readings[SENSOR_ARRAY_SIZE] = {0.0};
retained int record_index = 0;

unsigned long stateTime = 0;

HttpClient http;

// Headers currently need to be set at init, useful for API keys etc.
http_header_t headers[] = {
    { "Content-Type", "application/json" },
    //  { "Accept" , "application/json" },
    { "Accept" , "*/*"},
    { NULL, NULL } // NOTE: Always terminate headers will NULL
};

http_request_t request;
http_response_t response;

STARTUP(System.enableFeature(FEATURE_RETAINED_MEMORY));

void read_and_save_sensor() {
    // Turn cellular on and attempt to connect to the network so we can grab the tower info
    stateTime = millis();
    Cellular.on();
    Cellular.connect();
    waitFor(Cellular.ready, 90000); // wait up to 90 seconds for the connection
    if (Cellular.ready()) {
        unsigned long elapsed = millis() - stateTime;
        Serial.printlnf("Cellular connection at %s UTC (in %lu milliseconds)", Time.timeStr().c_str(), elapsed);

        CellularHelperEnvironmentResponse envResp = CellularHelper.getEnvironment(3);
        if (envResp.service.isValid()) {
            envResp.serialDebug();
            sensor_record[record_index].cell_mcc = envResp.service.mcc;
            sensor_record[record_index].cell_mnc = envResp.service.mnc;
            sensor_record[record_index].cell_lac = envResp.service.lac;
            sensor_record[record_index].cell_ci = envResp.service.ci;
        }
    }
    sensor_record[record_index].timestamp_unix = Time.now();
    FuelGauge().quickStart();
    delay(200);
    sensor_record[record_index++].state_of_charge = FuelGauge().getSoC();
}

void publish_sensor_readings() {
    request.hostname = "morfpad.hopto.org";
    request.port = 8080;
    request.path = "/api/spyberry";

    for (int i = 0; i < SENSOR_RECORD_ARRAY_SIZE; i++) {
      request.body = String::format(
          "{\"deviceID\":\"%s\", \
            \"scanDate\":\"%s\", \
            \"cell_mcc\":\"%d\", \
            \"cell_mnc\":\"%d\", \
            \"cell_lac\":\"%d\", \
            \"cell_ci\":\"%d\", \
            \"state_of_charge\":\"%.2f\"}", \
            System.deviceID().c_str(), \
            Time.format(sensor_record[i].timestamp_unix, TIME_FORMAT_ISO8601_FULL).c_str(), \
            sensor_record[i].cell_mcc, \
            sensor_record[i].cell_mnc, \
            sensor_record[i].cell_lac, \
            sensor_record[i].cell_ci, \
            sensor_record[i].state_of_charge);

        // Post the data
        http.post(request, response, headers);
        Serial.print("Application>\tResponse status: ");
        Serial.println(response.status);

        // Publish the data
        String results = "";
        results += String::format("%d, %.2f", sensor_record[i].timestamp_unix, sensor_record[i].state_of_charge);
        Serial.printlnf("Publishing : %s", results.c_str());
        bool success;
        success = Particle.publish("sensor_readings", results);
        if (!success) {
            Serial.println("Publish failed!");
        }
        delay(1000);
    }
    // Reset sensor_record
    memset(sensor_record, 0, sizeof(sensor_record));
    record_index = 0;
    delay(5000); // should not need this after 0.6.1 is released, ensures publish goes out before we sleep
}

void is_time_to_publish() {
    if (record_index >= SENSOR_RECORD_ARRAY_SIZE) {
        stateTime = millis();
        Particle.connect();
        waitFor(Particle.connected, 60000); // wait up to 60 seconds for a cloud connection
        if (Particle.connected()) {
            unsigned long elapsed = millis() - stateTime;
            Serial.printlnf("Particle Cloud connection at %s UTC (in %lu milliseconds)", Time.timeStr().c_str(), elapsed);
            publish_sensor_readings();
        }
    }
}

void setup()
{
    Serial.begin(9600);

    // Wake from deep sleep with power to cellular modem off.
    // Read sensor, store results to a retained array.
    read_and_save_sensor();

    // Is it time to publish our results?
    // Connect to the Cloud and publish.
    is_time_to_publish();

    // Go back to deep sleep - sleep time varies based on how long we were awake for
    // to keep time between readings close to 1 sampling interval - improve with Time class.
    // NOTE: this following command does not work in multithreaded mode yet,
    // but we don't need multithreading for this simple example
    if (SENSOR_READING_INTERVAL > (millis() / 1000)) {
        Serial.printlnf("Going back to sleep for : %d seconds", SENSOR_READING_INTERVAL - (millis() / 1000));
        System.sleep(SLEEP_MODE_SOFTPOWEROFF, SENSOR_READING_INTERVAL - (millis() / 1000));
    } else {
        Serial.printlnf("Going back to sleep for : %d seconds", SENSOR_READING_INTERVAL);
        System.sleep(SLEEP_MODE_SOFTPOWEROFF, SENSOR_READING_INTERVAL);
    }
}

void loop()
{
    Particle.process(); // only required when using MANUAL mode and if we ever use loop()
}
