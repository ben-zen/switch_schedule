// Copyright (C) 2019 Ben Lewis <benjf5+github@gmail.com>
// Licensed under an MIT license.
//#define TEST

#include <ESP8266WiFi.h>
#include <iterator>
#include <WiFiUdp.h>

// We need the system's MAC address.
byte mac[] = { 0xAC, 0xDE, 0x48, 0x23, 0x45, 0x67 };

unsigned long last_update_tick = 0;
unsigned long base_time = 0;

constexpr int time_zone_offset = -8; // This doesn't take half-hour time zones
                                     // into account. For my use, I don't really
                                     // care. A bigger issue is not being able
                                     // to do PST/PDT conversion without a
                                     // reflash. I'm still working that out.

constexpr unsigned int local_port = 123; // IANA-allocated NTP port.

constexpr char server_url[] = "time.windows.com";

constexpr char network_name[] = "network-name";
constexpr char network_key[] = "network-key";

constexpr int ntp_packet_size = 48;

byte packet_buffer[ntp_packet_size] = { };

WiFiUDP udp;

struct calendar_time {
  char day_of_week;
  short hours;
  short minutes;
  short seconds;

  bool operator < (calendar_time &that) {
    if (this->day_of_week < that.day_of_week) {
      // Earlier day, so yes.
      return true;
    } else if (this->day_of_week > that.day_of_week) {
      // Later day, so no.
      return false;
    }

    // Same day! Now we compare hours.
    if (this->hours < that.hours) {
      return true;
    } else if (this->hours > that.hours) {
      return false;
    }

    if (this->minutes < that.minutes) {
      return true;
    } else if (this->minutes > that.minutes) {
      return false;
    }

    if (this->seconds < that.seconds) {
      return true;
    }
    // Otherwise, it's the same time or later.
    return false;
  }
};

enum class switch_state : bool {
  off,
  on
};

constexpr int switch_state_to_pin_level(switch_state state) {
  return (state == switch_state::on) ? HIGH : LOW;
}

struct calendar_event {
  calendar_time time;
  switch_state state;
};

calendar_event calendar[] = {
  { { 0, 8, 0, 0 }, switch_state::on } ,
  { { 0, 18, 0, 0 }, switch_state::off } ,
  { { 1, 8, 0, 0 }, switch_state::on } ,
  { { 1, 18, 0, 0 }, switch_state::off } ,
  { { 2, 8, 0, 0 }, switch_state::on } ,
  { { 2, 18, 0, 0 }, switch_state::off } ,
  { { 3, 8, 0, 0 }, switch_state::on } ,
  { { 3, 18, 0, 0 }, switch_state::off } ,
  { { 4, 8, 0, 0 }, switch_state::on } ,
  { { 4, 18, 0, 0 }, switch_state::off } };
calendar_event *last_event = nullptr;

switch_state switch_value = switch_state::off;

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200); // Setting a baud to communicate over.
  Serial.println();

#ifndef TEST
  Serial.write("Connecting to WiFi network: ");
  Serial.write(network_name);
  Serial.println();
  WiFi.begin(network_name, network_key);
  Serial.write("Connected!\r\n");
  udp.begin(local_port);

  // Set up lines to listen on and write to.
  pinMode(12, OUTPUT); // GPIO12 goes to the relay in a Sonoff switch.
  pinMode(13, OUTPUT); // GPIO13 is the case LED. Can be used for whatever.
  pinMode(14, INPUT); // GPIO14 is routed to an empty pin broken out on the
                      // PCB.

  pinMode(0, INPUT_PULLUP); // GPIO0 is the button on a Sonoff switch
  // When the button's pushed, toggle the state.
  attachInterrupt(digitalPinToInterrupt(0), handle_button_press, FALLING);
#endif
}

// Return a duration of hours in seconds.
constexpr long hours(int hour_count) {
  return hour_count
    * 60 /* minutes */
    * 60; /* seconds */
}

int day_of_week(unsigned long time) {
  // Time is an offset from 0:00 1 Jan 1900.
  // We want the number of days since that epoch, so we'll divide by the number
  // of seconds in a day.
  auto days = time / hours(24);
  // With the number of days, we can now take the modulus of this with 7, to
  // get the week-offset point of today compared to the day of the week that
  // 1 Jan 1900 was; conveniently, that was a Monday. By treating Monday as the
  // 0th entry in the weekly schedule calendar, we can easily find the current
  // schedule.
  return days % 7;
}

char const *day_of_week_s(int day) {
  switch (day) {
  case 0:
    return "Monday";
  case 1:
    return "Tuesday";
  case 2:
    return "Wednesday";
  case 3:
    return "Thursday";
  case 4:
    return "Friday";
  case 5:
    return "Saturday";
  case 6:
    return "Sunday";
  }

  return "";
}

unsigned long current_time(unsigned long long current_offset) {
  // If current_offset < last_update_tick, we've had a reset, and can put a
  // message over serial, and that's about it. We'll also at least apply the
  // offset to the last_update_tick.
  if (current_offset < last_update_tick) {
    Serial.write("Counter has overflowed, experiencing reset.\r\n");
    current_offset = last_update_tick + current_offset;
  }

  // We want to return the base time plus the current offset in seconds, i.e.
  // divided by a thousand.
  return base_time + (current_offset / 1000);
}

calendar_time wall_time_to_calendar_time(unsigned long long wall_time) {
  calendar_time time { };
  time.day_of_week = day_of_week(wall_time);

  auto time_of_day = wall_time % hours(24);
  time.hours = time_of_day / hours(1);
  time.minutes = (time_of_day % hours(1)) / 60;
  time.seconds = time_of_day % 60;
  return time;
}

struct calendar_event *get_last_event(unsigned long long current_time) {
  calendar_event *last_event = nullptr;
  auto current_cal_time = wall_time_to_calendar_time(current_time);
  for (auto event = std::begin(calendar); event != std::end(calendar); event++) {
    last_event = event;
    if ((event + 1) != std::end(calendar)) {
      auto next = event + 1;
      if ((current_cal_time < next->time) && (event->time < current_cal_time)) {
        // Only break if the current time is before the next event and after the
        // last event. If the current time is after (or before) all events in
        // the calendar, this will force a search to the end of the list, which
        // finds the last event of the prior week, i.e. the last event.
        break;
      }
    }
  }

  return last_event;
}

bool should_update_time(unsigned long long current_time) {
  // If we've gone past 24 hours since the last update, or the time has never
  // been updated (last_update_tick is 0), _or_ the clock has rolled over
  // (which will happen every 50 days), we need to update the time.

  if (current_time < last_update_tick) {
    Serial.write("The clock has reset since the last update!\r\n");
    return true;
  } else if (last_update_tick == 0) {
    return true;
  } else if ((current_time - last_update_tick) >= (hours(24) * 1000)) {
    return true;
  }

  return false;
}

void handle_button_press() {
  auto current_state = digitalRead(12);
  digitalWrite(12, (current_state == HIGH) ? LOW : HIGH);
}

void loop() {
#ifdef TEST
    // Run some tests of the parsing logic, amongst other questions.
  Serial.write("Testing mode. Running tests.\r\n");

  Serial.write("Testing day_of_week()\r\n");
  // Test the day-of-week code. 2019-01-19 is a Saturday, i.e. `5` in the
  // program calendar, and the provided integer is from that day, in UTC.
  auto test_date = day_of_week(3756876735);

  Serial.write("Expected value: 5. Actual value: ");
  Serial.println(test_date);
  Serial.write((test_date == 5) ? "PASS\r\n\r\n" : "FAIL\r\n\r\n");

  Serial.write("Testing should_update_time()\r\n");
  Serial.write("last_update_tick is 0; "
    "should_update_time() should return true.\r\n");
  last_update_tick = 0;
  Serial.write(should_update_time(1000) ? "PASS\r\n\r\n" : "FAIL\r\n\r\n");

  Serial.write("last_update_tick is non-zero, current_time is more than 24h "
    " apart. should_update_time() should return true.\r\n");
  last_update_tick = 1000;
  Serial.write(should_update_time(hours(25) * 1000) ? "PASS\r\n\r\n"
                : "FAIL\r\n\r\n");
  Serial.write("last_update_tick is greater than current_time. "
    "should_update_time() should return true.\r\n");
  Serial.write(should_update_time(500) ? "PASS\r\n\r\n" : "FAIL\r\n\r\n");

  Serial.write("current_time is greater than last_update_tick, but not 24h "
    "past. should_update_time() should return false.\r\n");
  Serial.write(should_update_time(1500) ? "FAIL\r\n\r\n" : "PASS\r\n\r\n");

  Serial.write("Testing current_time()\r\n");
  last_update_tick = 5000;
  Serial.write("last_update_tick is ");
  Serial.println(last_update_tick);
  // Set the base time
  base_time = 32500;
  Serial.write("base_time is ");
  Serial.println(base_time);

  // For an offset of 10000, we expect the result to be 32510, since offset is
  // expressed in milliseconds while NTP time (and therefore this test's time)
  // is expressed in seconds.
  auto comp_time = current_time(10000);
  Serial.write("For an offset of 10000, expected time is 32510. Actual time is ");
  Serial.println(comp_time);
  Serial.write(comp_time == 32510 ? "PASS\r\n\r\n" : "FAIL\r\n\r\n");

  // Now we'll test the offset being skew from last_update_tick.
  comp_time = current_time(1000);
  Serial.write("For an offset of 1000, expected time is 32506. Actual time is ");
  Serial.println(comp_time);
  Serial.write(comp_time == 32506 ? "PASS\r\n\r\n" : "FAIL\r\n\r\n");

  Serial.write("Done with tests. Sleeping for 30s.\r\n");
  delay(30000);
#else // ifdef TEST
  Serial.write("Normal operation.\r\n");
  auto current_offset = millis();
  if (base_time) {
    Serial.write("Current time is: ");
    print_time(current_time(current_offset));
    Serial.println();
  }

  if (should_update_time(current_offset)) {
    Serial.write("Requesting a time update from NTP.\r\n");
    if (!send_ntp_request()) {
      Serial.write("Failed to send request. Waiting 10 seconds then "
		   "retrying.\r\n");
      delay(10000);
      return;
    }

    // Wait a second for a response from the server.
    delay(1000);
    if (udp.parsePacket()) {
      // We've received a packet, let's pull our data from it.
      udp.read(packet_buffer, ntp_packet_size);

      Serial.write("Received an NTP update.\r\n");
      // We really care about the timestamp at byte 40. It's 4 bytes long.
      unsigned long timestamp = (((unsigned long)packet_buffer[40]) << 24)
        + (((unsigned long)packet_buffer[41]) << 16)
        + (((unsigned long)packet_buffer[42]) << 8)
        + packet_buffer[43];

      Serial.write("Received timestamp: ");
      Serial.println(timestamp);

      if (timestamp < base_time) {
        Serial.write("Clock skew encountered!\r\n");
      }

      base_time = timestamp + hours(time_zone_offset);
      last_update_tick = millis();

      Serial.write("Updated time at: ");
      print_time(current_time(last_update_tick));
      Serial.println();
    }
  }

  // Now, the light control.
  current_offset = millis();
  auto prev_event = last_event;
  auto current_event = get_last_event(current_time(current_offset));

  Serial.print("The last event was on ");
  Serial.print(day_of_week_s(current_event->time.day_of_week));
  Serial.print(", at ");
  print_time(current_event->time);
  Serial.print(", and set the switch ");
  Serial.print(current_event->state == switch_state::on ? "on." : "off.");
  Serial.println();

  if (current_event != prev_event) {
    last_event = current_event;
    switch_value = current_event->state;
    digitalWrite(12, switch_state_to_pin_level(switch_value));
  }

  delay(30000); // Wait 10 seconds before retrying anything.
#endif
}

bool send_ntp_request() {
  memset(packet_buffer, 0, ntp_packet_size);

  packet_buffer[0] = 0b00100011; // LI: 0b00, VN: 0b100, Mode: 0b011

  // Open a port to the time server on the NTP port.
  if (udp.beginPacket(server_url, 123)) {
    Serial.write("Began message to time server ");
    Serial.write(server_url);
    Serial.println();

    udp.write(packet_buffer, sizeof(packet_buffer));
    udp.endPacket();
    return true;
  } else {
    Serial.write("Error starting message!\r\n"); // Has a hard time with this
                                                 // idea at first.
    return false;
  }
}

void print_time(unsigned long timestamp) {
  print_time(wall_time_to_calendar_time(timestamp));
}

void print_time(calendar_time time) {
  if (time.hours < 10) {
    Serial.print(0);
  }
  Serial.print(time.hours);
  Serial.print(":");

  if (time.minutes < 10) {
    Serial.print(0);
  }
  Serial.print(time.minutes);
  Serial.print(":");

  if (time.seconds < 10) {
    Serial.print(0);
  }
  Serial.print(time.seconds);
}
