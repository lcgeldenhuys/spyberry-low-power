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

const unsigned long STARTUP_WAIT_TIME_MS = 8000;
bool startup_time_is_valid = TRUE;

// Time between sensor readings in seconds
#define SENSOR_READING_INTERVAL 60*3
// How many records to keep if we can't get a connection
#define SENSOR_RECORD_ARRAY_SIZE 1
typedef struct
{
    unsigned long   recordNum;
    time_t          timestamp_unix;
    unsigned long   timeCellularConnect;
    int             cell_mcc;
    int             cell_mnc;
    int             cell_lac;
    int             cell_ci;
    float           state_of_charge;
    // 32 bytes (the cell params could be uint_16 if needed)
} SensorRecordDef;
// If we lose power completely, expect these to reinitialize.
retained SensorRecordDef sensor_record[SENSOR_RECORD_ARRAY_SIZE] = {0};
retained unsigned long record_index = 0;
retained time_t lastSyncTimestamp = 0;

enum State {
  	STARTUP_WAIT_STATE,
  	CONNECT_STATE,
    READ_SENSOR_STATE,
  	COLLECT_CELL_INFO,
  	POST_TO_CLOUD_STATE,
    PUBLISH_DIAGS_STATE,
  	GO_TO_SLEEP_STATE
};
State state = STARTUP_WAIT_STATE;
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
    // Grab sensor data, for now only the FuelGauge
    FuelGauge().quickStart();
    delay(200);
    sensor_record[record_index].state_of_charge = FuelGauge().getSoC();
}

bool read_and_save_cell_info() {
    CellularHelperEnvironmentResponse envResp = CellularHelper.getEnvironment(3);
    if (envResp.service.isValid()) {
        //envResp.serialDebug();
        sensor_record[record_index].cell_mcc = envResp.service.mcc;
        sensor_record[record_index].cell_mnc = envResp.service.mnc;
        sensor_record[record_index].cell_lac = envResp.service.lac;
        sensor_record[record_index].cell_ci = envResp.service.ci;
        return TRUE;
    }
    else {
        Serial.println("Cell info failed!");
        return FALSE;
    }
}

bool post_to_cloud() {
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
        if (response.status != 200) {
            Serial.println("Post failed!");
            Serial.print("Application>\tResponse status: ");
            Serial.println(response.status);
            return FALSE;
        }
        return TRUE;
    }
    // Reset sensor_record
    //memset(sensor_record, 0, sizeof(sensor_record));
    //record_index = 0;
    //delay(5000); // should not need this after 0.6.1 is released, ensures publish goes out before we sleep
}

bool publish_to_particle() {
    Particle.connect();
    waitFor(Particle.connected, 60000); // wait up to 60 seconds for a cloud connection
    if (Particle.connected()) {
        for (int i = 0; i < SENSOR_RECORD_ARRAY_SIZE; i++) {
            //Particle.process();
            Serial.print(Time.isValid());
            Serial.println("\n");
            // Publish the data
            String results = "";
            results += String::format("%d, %.2f", sensor_record[i].timestamp_unix, sensor_record[i].state_of_charge);
            //Serial.printlnf("Publishing : %s", results.c_str());
            bool success;
            success = Particle.publish("sensor_readings", results);
            if (!success) {
                Serial.println("Publish failed!");
            }
            //delay(5000);
            return TRUE;
        }
    }
    else {
        return FALSE;
    }
}

void setup()
{
    Serial.begin(9600);
    Serial.println("\n");

    // Check if RTC is valid (later than Aptil 9th 2017) so we can force Particle time sync
    if (Time.timeStr() < 1491710163) {
        startup_time_is_valid = FALSE;
    }
}

void loop()
{
    switch(state) {
    case STARTUP_WAIT_STATE:
        // Wake from deep sleep with power to cellular modem off.
        // Initialise new data record.
        if (millis() - stateTime >= STARTUP_WAIT_TIME_MS) {
            stateTime = millis();
            state = READ_SENSOR_STATE;
        }
        sensor_record[record_index].recordNum = record_index;
        sensor_record[record_index].timestamp_unix = Time.now();
        break;

    case READ_SENSOR_STATE:
        // Read sensors, store results to the retained array.
        Serial.printlnf("\n==> %lu\t\t%s\tWaking up, record num : %lu", millis(), Time.timeStr().c_str(), record_index);
        read_and_save_sensor();
        state = CONNECT_STATE;
        stateTime = millis();
        break;

    case CONNECT_STATE:
        // Turn modem on and connect to the network.
        Cellular.on();
        //Serial.println("Attempting to connect to the cellular network...");
        Cellular.connect();
        waitFor(Cellular.ready, 90000);
        if (Cellular.ready()) {
            unsigned long elapsed = millis() - stateTime;
            Serial.printlnf("==> %lu\t%lu\t%s\tCellular network connection", millis(), elapsed, Time.timeStr().c_str());
            sensor_record[record_index].timeCellularConnect = elapsed;
        }
        else {
            // Timeout connecting to cellular network, log to diags and go to sleep.
            Serial.println("Timeout connecting to the cellular network");
            state = GO_TO_SLEEP_STATE;
            break;
        }
        state = COLLECT_CELL_INFO;
        stateTime = millis();
        break;

    case COLLECT_CELL_INFO:
        // Collect cell tower information and store to retained array.
        if (read_and_save_cell_info()) {
            unsigned long elapsed = millis() - stateTime;
            Serial.printlnf("==> %lu\t%lu\t%s\tCell info collected", millis(), elapsed, Time.timeStr().c_str());
        }
        else {
            // Failed to get cell info, log to diags and proceed to post
        }
        state = POST_TO_CLOUD_STATE;
        stateTime = millis();
        break;

    case POST_TO_CLOUD_STATE:
        // Post valid records to the cloud.
        if (post_to_cloud()) {
            unsigned long elapsed = millis() - stateTime;
            Serial.printlnf("==> %lu\t%lu\t%s\tPosted to cloud", millis(), elapsed, Time.timeStr().c_str());
        }
        else {
            // Failed to post to cloud, log to diags and then proceed to publish
        }
        state = PUBLISH_DIAGS_STATE;
        stateTime = millis();
        break;

    case PUBLISH_DIAGS_STATE:
        // Connect to the Particle Cloud and publish.
        if (publish_to_particle()) {
            unsigned long elapsed = millis() - stateTime;
            stateTime = millis();
            Serial.printlnf("==> %lu\t%lu\t%s\tPublished diags to Particle", millis(), elapsed, Time.timeStr().c_str());
            Particle.process();
        }
        else {
            // Failed to publish ...
        }

        if (!startup_time_is_valid) {
            // Request time synchronization from the Particle Cloud
            Particle.syncTime();
            // Wait until Electron receives time from Particle Cloud (or connection to Particle Cloud is lost)
            waitUntil(Particle.syncTimeDone);
            if (Time.isValid()) {
                // Print current time
                elapsed = millis() - stateTime;
                Serial.printlnf("\n==> %lu\t%lu\t%s\tStartup time sync at record num : %lu", millis(), elapsed, Time.timeStr().c_str(), record_index);
                startup_time_is_valid = TRUE;
            }
            else {
                // Failed, log somehow?
            }
        }

        state = GO_TO_SLEEP_STATE;
        break;

    case GO_TO_SLEEP_STATE:
        // Bump the record index
        record_index++;

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

    //Particle.process(); // only required when using MANUAL mode and if we ever use loop()
}
