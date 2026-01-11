# GridPower
A small repo for a zigbe device I built reading the red light impulse on the power meter so I can monitor usage - Zigbee on esp32c6 DFRobot Beatle DFR1117

#
Board I used was
https://wiki.dfrobot.com/SKU_DFR1117_Beetle_ESP32_C6

Solder 6 pins - only 4 but may end up adding dht11 and door sensor to board

#
Connect GPIO 4 and 3.3v to the LDR, its an LDR so it doesnt matter which leg is which, and by setting the GPIO pin to type PULLUP you get a free resister to ground

So, to say that in another way. You can wire up either leg to either side just make sure one leg is on GPIO4 and one is on 3.3v.

NOTE: THis works on these esp32c6 chips BECASUE we set the pin to PULLUP which uses the internal resister.

#
System internal resisters does enough to debounce reflections and sets the edge of the current sharp enough to detect.

I put the resister inside the cap of a milk bottle and covered it in black electric tape.

Connect 18650 Battery +/- on BAT/GND pins (opposite side of the board)
