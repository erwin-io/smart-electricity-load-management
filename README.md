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



Platform and library setup

https://raw.githubusercontent.com/erwin-io/smart-electricity-load-management/refs/heads/main/platformio.ini

Device storage setup
https://raw.githubusercontent.com/erwin-io/smart-electricity-load-management/refs/heads/main/partitions.csv

Bbackend Server code files

https://raw.githubusercontent.com/erwin-io/smart-electricity-load-management/refs/heads/main/src/main.cpp


UI code files

https://raw.githubusercontent.com/erwin-io/smart-electricity-load-management/refs/heads/main/data/login.html

https://raw.githubusercontent.com/erwin-io/smart-electricity-load-management/refs/heads/main/data/logs.html

https://raw.githubusercontent.com/erwin-io/smart-electricity-load-management/refs/heads/main/data/logs_print.html

https://raw.githubusercontent.com/erwin-io/smart-electricity-load-management/refs/heads/main/data/configuration.html

https://raw.githubusercontent.com/erwin-io/smart-electricity-load-management/refs/heads/main/data/dashboard.html

https://raw.githubusercontent.com/erwin-io/smart-electricity-load-management/refs/heads/main/data/index.html

UI logic code files


https://raw.githubusercontent.com/erwin-io/smart-electricity-load-management/refs/heads/main/data/js/app.js

https://raw.githubusercontent.com/erwin-io/smart-electricity-load-management/refs/heads/main/data/js/config.js

https://raw.githubusercontent.com/erwin-io/smart-electricity-load-management/refs/heads/main/data/js/dashboard.js

https://raw.githubusercontent.com/erwin-io/smart-electricity-load-management/refs/heads/main/data/js/login.js

https://raw.githubusercontent.com/erwin-io/smart-electricity-load-management/refs/heads/main/data/js/logs.js

UI Styles colors and fonts codes

https://raw.githubusercontent.com/erwin-io/smart-electricity-load-management/refs/heads/main/data/css/styles.css