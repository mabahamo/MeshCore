t100e:
	pio run -e t1000e_companion_radio_ble -t create_uf2

emergency:
	pio run -e Heltec_v3_trigger_emergency

emergency-upload:
	pio run -e Heltec_v3_trigger_emergency -t upload

emergency-erase:
	@echo "Erasing SPIFFS partition to factory reset the device..."
	pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 erase_region 0x310000 0xF0000
	@echo "Factory reset complete. Press RESET button on device to restart."

monitor:
	pio device monitor -b 115200 -p /dev/tty.usbserial-0001
