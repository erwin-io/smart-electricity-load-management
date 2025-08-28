# smart-electricity-load-management
smart-electricity-load-management


wiring


P1 (×3): 12 (activeHigh=true, add ~10k pulldown), 15 (activeHigh=false), 2 (activeHigh=false)

P2 (×3): 17 (true), 32 (true), 33 (true)

P3 (×4): 13 (true), 14 (true), 16 (true), 27 (true)

P4 (×1): 4 (true)

SD (SPI): SCK=18, MISO=19, MOSI=23, CS=5

RTC (I²C): SDA=21, SCL=22

PZEM (UART2): RX2=26 (from PZEM TX via divider), TX2=25 (to PZEM RX)