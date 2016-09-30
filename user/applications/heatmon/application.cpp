/*********************************************************************************
 Copyright (C) 2014 by Stefan Filipek

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
*********************************************************************************/

#include <array>
#include <list>

// Make sure to include the C++ headers above here, as min() and max() defines
// will screw up the stdlib
#include "application.h"

#define PARTICLE_VARIABLE_SIZE (12)

SYSTEM_MODE(AUTOMATIC);

/**
 * Hysteresis values for polling the input lines
 * We are polling a 60Hz signal when the zone demand is on. Fortunately, the
 * probability of seeing "high" on the line during this time is low, so we can
 * do a simple average over time, hysteresis, or other debounce technique.
 *
 * Let's use hysteresis...
 *
 * A low count will mean that the zone is off. A high count means the zone
 * demand is on. Use a simple threshold detector, evenly spaced.
 */
const int HYST_MAX          = 500;
const int HYST_MIN          = 0;
const int HYST_THRESH_ON    = 400; // Turn on when count reaches 20
const int HYST_THRESH_OFF   = 100; // Turn off when count falls below 10

enum Demand{
    Unknown,
    True,
    False 
};

/**
 * Zone information/configuration
 */
struct ZoneInfo {
    int id;
    uint16_t pin;
    int count;
    int demand;
};


/**
 * Zone events -- when a zone turned on or off
 */
struct ZoneEvent {
    ZoneInfo info;
    int time;
    int demand;
};



int polls;          // Just FYI, number of times the zones have been polled
int last_poll;      // Last time data was polled from the zones
int last_event;     // Last time an event occurred


// Iterable list of zones
std::array<ZoneInfo, 3> zone_list{
    ZoneInfo{0, D4, HYST_MAX/2, Demand::Unknown},
    ZoneInfo{1, D5, HYST_MAX/2, Demand::Unknown},
    ZoneInfo{2, D6, HYST_MAX/2, Demand::Unknown},
//    ZoneInfo{3, D7, HYST_MAX/2, Demand::Unknown},
};


// Keep a lot of events so we can update anyone after a connection loss
// This is all we can really fit in the JSON string buffer below
static const size_t MAX_NUM_EVENTS = 20;
std::list<ZoneEvent> zone_events;


// Allow for retrieval of all past events recorded by this device
// Well... not ALL, but a fair amount. About 20.
// Max variable string is 622 bytes... let's not push it allll the way...
char zone_events_json[620] = "[]";
const char * const ZONE_JSON_STRING = "{\"id\":%d,\"t\":%d,\"on\":%d}";



/**
 * Attempt to write a complete JSON string to a given buffer, including NULL.
 *
 * @param event: ZoneEvent 
 * @param buffer: Output character buffer
 * @param buffer_size: Number of bytes available in the buffer
 * @param leading_comman: If true, prepend a comma to the string (if there's space)
 * 
 * @return Number of bytes written, or -1 on failure.
 *
 * In the even of failure, a partial JSON object will have been written.
 * It is up to the caller to clean up the string buffer after the event.
 *
 * If there wasn't room for both the JSON string AND a NULL terminator, then
 * a failure (-1) will be returned.
 */
int write_event_json(const ZoneEvent & event,
        char* buffer, size_t buffer_size,
        bool leading_comma=false)
{
    if(leading_comma && buffer_size >= 2) {
        *buffer = ',';
        buffer++;
        buffer_size--;
    }

    size_t needed = snprintf(buffer, buffer_size, ZONE_JSON_STRING,
            event.info.id, event.time, event.demand == Demand::True);

    // See if we could fit the string AND an extra terminating NULL
    if(needed >= buffer_size) {
        return -1;
    }
    return needed + (int)leading_comma;
}


/**
 * Write all logged events to the zone_events_json buffer.
 *
 * This function will write all events (as much as can fit) into the buffer.
 * Tested on 2014-10-20 using various buffer sizes.
 * Works... no buffer overflows. Yay.
 */
int update_event_json(void) {
    bool first = true;
    size_t idx = 0;
    
    // Min bound, just incase we screw up the definition of the buffer...
    if(sizeof(zone_events_json) < 3) return 0;

    // Start the array
    zone_events_json[idx++] = '[';

    // Write a JSON object for each zone event
    for(auto & event : zone_events) {
        // We need at least 1 extra byte for the ending ']'
        // (NULL is implied in write_event_json)
        int free_space = sizeof(zone_events_json) - idx - 1;
        int written = write_event_json(event, zone_events_json+idx, free_space, !first);
        first = false;

        if(written < 0) {
            break;
        }

        idx += written;
    }


    // Terminate the array
    zone_events_json[idx++] = ']';

    // Always null terminate
    zone_events_json[idx] = 0;

    return idx;
}


/**
 * Poll the status of all zones in our system
 */
void read_all_zones(void) {

    bool new_event = false;

    char buff[64];

    // Iterate over every zone that we have
    for(auto & zone : zone_list) {
        Demand previous_demand = (Demand)zone.demand;

        // Note: HIGH means zone is off, LOW means it is on
        if(digitalRead(zone.pin) == HIGH) {
            zone.count -= 1;

            // Boundary checks and status updates
            if(zone.count < HYST_MIN) zone.count = HYST_MIN;
            if(zone.count < HYST_THRESH_OFF) zone.demand = (int)Demand::False;
            
        }else{
            zone.count += 5;

            // Boundary checks and status updates
            if(zone.count > HYST_MAX) zone.count = HYST_MAX;
            if(zone.count > HYST_THRESH_ON) zone.demand = (int)Demand::True;
        }

        // Change in status? Log an event (and publish it)
        if(previous_demand != zone.demand) {
            new_event = true;

            int event_time = Time.now();

            // Add to our queue at the front (most recent goes first)
            if(zone_events.size() == MAX_NUM_EVENTS) zone_events.pop_back();
            zone_events.emplace_front(
                    ZoneEvent{zone, event_time, zone.demand});

            // Publish
            write_event_json(zone_events.front(), buff, sizeof(buff));
            Particle.publish("zone_demand", buff, PRIVATE);
        }
    }

    int time = Time.now();
    last_poll = time;

    // Increase poll count, with rollover to 0 (instead of negative)
    polls += 1;
    if(polls < 0) polls = 0;

    if(new_event) {
        update_event_json();
        last_event = time;
    }
}

/**
 * Send a monotically increasing heartbeat value to test the network connectivity.
 */
void send_heartbeat() {
    static unsigned long hb = 0;
    Particle.publish("heartbeat", String(hb++), PRIVATE);
}

/**
 * Synchronize our clock with the network.
 */
void sync_time() {
    Particle.syncTime();
}


/**
 * Generic function to rate-limit a standard function call.
 *
 * This takes care of keeping track of the last time a function was called.
 */
template<unsigned long ACTION_DELAY_MS, void (*function)()>
void rate_limited_call() {
    static unsigned long last_action = 0;
    unsigned long now = millis();
    if((now - last_action) < ACTION_DELAY_MS) return;

    last_action = now;
    function();
}


void setup()
{
    // Variable name buffer
    char buff[PARTICLE_VARIABLE_SIZE];

    // Set the sense line to be an input for all zones
    // Also register some fancy-schmancy variables for web debug
    for(ZoneInfo& zone : zone_list) {

        pinMode(zone.pin, INPUT);

        snprintf(buff, sizeof(buff), "z%d_demand", zone.id);
        Particle.variable(buff, zone.demand);

        snprintf(buff, sizeof(buff), "z%d_count", zone.id);
        Particle.variable(buff, zone.count);
    }
   
    Particle.variable("zone_events",    zone_events_json);
    Particle.variable("polls",          polls);
    Particle.variable("last_poll",      last_poll);
    Particle.variable("last_event",     last_event);
}


void loop()
{
    static const unsigned long HALF_DAY_MS = 12*60*60*1000;
    static const unsigned long MINUTE_MS   = 60*1000;

    rate_limited_call<10,           read_all_zones >();
    rate_limited_call<MINUTE_MS,    send_heartbeat >();
    rate_limited_call<HALF_DAY_MS,  sync_time      >();
}

