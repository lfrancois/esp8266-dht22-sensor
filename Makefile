all:
	platformio run --target upload

clean:
	platformio run --target clean

serial:
	platformio serialports monitor
