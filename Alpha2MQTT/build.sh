#!/bin/bash 
#  export GIT_BASE=/volume1/docker_cached/apps/vscode/git/Alpha2MQTT
#  docker run -d --name arduino-cli-build -v  ${GIT_BASE}:/project --entrypoint tail solarbotics/arduino-cli:0.25.0-python3.7 -f /dev/null
#  chmod -R a+rwX $GIT_BASE/Alpha2MQTT/build/
#  docker exec -it arduino-cli-build /project/Alpha2MQTT/build.sh

arduino-cli core update-index --additional-urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli core install esp8266:esp8266@3.0.2  --additional-urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli lib install "Adafruit BusIO"
arduino-cli lib install "Adafruit SSD1306"
arduino-cli lib install "Adafruit GFX Library"
arduino-cli lib install PubSubClient

cd /project
for SER in 63 54
do
    echo "Building for ${SER}"
    arduino-cli  compile -e --build-property build.extra_flags="-DESP8266  -DDEVICE_NAME=\"Alpha2MQTT-${SER}\"" --fqbn esp8266:esp8266:nodemcuv2 Alpha2MQTT
    mv Alpha2MQTT/build/esp8266.esp8266.nodemcuv2/Alpha2MQTT.ino.bin ./Alpha2MQTT-${SER}.bin
done