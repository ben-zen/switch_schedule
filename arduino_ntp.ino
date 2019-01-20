
//#define TEST

#include <ESP8266WiFi.h>
#include <WifiUdp.h>

// We need the system's MAC address.
byte mac[] = { 0xAC, 0xDE, 0x48, 0x23, 0x45, 0x67 };

unsigned long last_update_time = 0;
unsigned long base_time = 0;

constexpr unsigned int local_port = 123; // IANA-allocated NTP port.

constexpr char server_url[] = "time.windows.com";

constexpr char network_name[] = "network-name";
constexpr char network_key[] = "network-key";

constexpr int ntp_packet_size = 48;

byte packet_buffer[ntp_packet_size] = { };

WiFiUDP udp;

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
#endif
}

// Return a duration of hours in seconds. 
constexpr unsigned long hours(unsigned int hour_count) {
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

unsigned long current_time(unsigned long long current_offset) {
  // If current_offset < last_update_time, we've had a reset, and can put a 
  // message over serial, and that's about it. We'll also at least apply the
  // offset to the last_update_time.
  if (current_offset < last_update_time) {
    Serial.write("Counter has overflowed, experiencing reset.\r\n");
    current_offset = last_update_time + current_offset;
  }

  // We want to return the base time plus the current offset in seconds, i.e.
  // divided by a thousand.
  return base_time + (current_offset / 1000);
}

// Determine what day of the week it is; we do this by taking the NTP time,
// applying the millisecond offset (rounded down),  and
// getting it down to days

bool should_update_time(unsigned long long current_time) {
  // If we've gone past 24 hours since the last update, or the time has never
  // been updated (last_update_time is 0), _or_ the clock has rolled over
  // (which will happen every 50 days), we need to update the time.

  if (current_time < last_update_time) {
    Serial.write("The clock has reset since the last update!\r\n");
    return true;
  } else if (last_update_time == 0) {
    return true;
  } else if ((current_time - last_update_time) >= (hours(24) * 1000)) {
    return true;
  }

  return false;
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
  Serial.write("last_update_time is 0; "
    "should_update_time() should return true.\r\n");
  last_update_time = 0;
  Serial.write(should_update_time(1000) ? "PASS\r\n\r\n" : "FAIL\r\n\r\n");

  Serial.write("last_update_time is non-zero, current_time is more than 24h "
    " apart. should_update_time() should return true.\r\n");
  last_update_time = 1000;
  Serial.write(should_update_time(hours(25) * 1000) ? "PASS\r\n\r\n" 
                : "FAIL\r\n\r\n");
  
  Serial.write("last_update_time is greater than current_time. "
    "should_update_time() should return true.\r\n");
  Serial.write(should_update_time(500) ? "PASS\r\n\r\n" : "FAIL\r\n\r\n");

  Serial.write("current_time is greater than last_update_time, but not 24h "
    "past. should_update_time() should return false.\r\n");
  Serial.write(should_update_time(1500) ? "FAIL\r\n\r\n" : "PASS\r\n\r\n");

  Serial.write("Testing current_time()\r\n");
  last_update_time = 5000;
  Serial.write("last_update_time is ");
  Serial.println(last_update_time);
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

  // Now we'll test the offset being skew from last_update_time.
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
    send_ntp_request();
    
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

      base_time = timestamp;
      last_update_time = millis();

      Serial.write("Updated time at: ");
      print_time(current_time(last_update_time));
      Serial.println();
    }
  }

  // Now, the light control.

  delay(10000); // Wait 10 seconds before retrying anything.
#endif
}

void send_ntp_request() {
  memset(packet_buffer, 0, ntp_packet_size);

  packet_buffer[0] = 0b00100011; // LI: 0b00, VN: 0b100, Mode: 0b011

  // Open a port to the time server on the NTP port.
  if (udp.beginPacket(server_url, 123)) {
    Serial.write("Began message to time server ");
    Serial.write(server_url);
    Serial.println();

    udp.write(packet_buffer, sizeof(packet_buffer));
    udp.endPacket();
  } else {
    Serial.write("Error starting message!\r\n"); // Has a hard time with this idea at first.
  }
}

void print_time(unsigned long timestamp) {
  // Do I care about the date? We can care about the date anther time.
  auto time_of_day = timestamp % hours(24);
  auto hour = time_of_day / hours(1);
  // First get just the remainder when divided by hours, then divide by 60
  // to get rid of seconds.
  // Next 
  auto minute = (time_of_day % hours(1)) / 60;
  auto second = time_of_day % 60;

  if (hour < 10) {
    Serial.print(0);
  }
  Serial.print(hour);
  Serial.print(":");

  if (minute < 10) {
    Serial.print(0);
  }
  Serial.print(minute);
  Serial.print(":");

  if (second < 10) {
    Serial.print(0);
  }
  Serial.print(second);
}