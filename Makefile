t100e:
	pio run -e t1000e_companion_radio_ble -t create_uf2

forwarder:
	pio run -e Heltec_v3_forwarder
	pio run -e Heltec_v3_forwarder -t upload --upload-port /dev/tty.usbserial-0001
