#!/bin/bash 
#  export GIT_BASE=/volume1/docker_cached/apps/vscode/git/Alpha2MQTT
#  docker run -d --name arduino-cli-build -v  ${GIT_BASE}:/project --entrypoint tail arduinoci/ci-arduino-cli:v1.3.1 -f /dev/null
#  chmod -R a+rwX $GIT_BASE/Alpha2MQTT/build/
#  docker exec -it arduino-cli-build /project/Alpha2MQTT/build.sh

arduino-cli core update-index --additional-urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli core install esp8266:esp8266@3.1.2  --additional-urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli lib install "Adafruit BusIO"
arduino-cli lib install "Adafruit SSD1306"
arduino-cli lib install "Adafruit GFX Library"
arduino-cli lib install "WiFiManager"
arduino-cli lib install "Preferences"
arduino-cli lib install PubSubClient

cd /project
arduino-cli  compile -e --build-property build.extra_flags="-DMP_ESP8266 -UMP_ESP32 -UMP_XIAO_ESP32C6" --fqbn esp8266:esp8266:d1_mini Alpha2MQTT
mv Alpha2MQTT/build/esp8266.esp8266.d1_mini/Alpha2MQTT.ino.bin ./Alpha2MQTT.bin
