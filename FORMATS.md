# ESP266 Homeyduino configuration file formats

Here is the specification of the configuration files that the ESP8266 Homeyduino uses.

## homey.txt

On a single line specify the device name used while pairing with Homey. Row must end in a newline.

**Example homey.txt file:**

```
my homey IoT device
```

## known_wifis.txt

One known WiFi network per row. First the SSID, then TAB, then password and newline.

**Example known_wifis.txt**

```
OH2MP	MyVerySecretPass123
OH2MP-5	AnotherVerySecretPass456
```
