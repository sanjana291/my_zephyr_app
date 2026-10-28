
# Hello World

## Description
A basic Zephyr application that prints the board target.

## Build
```bash
west build -b imxrt1176_som/mimxrt1176/cm7 .
```

## UART Connection
- Connect J4 debug connector to the MCU-Link Debug Probe UART interface.
- Open Tera Term on the PC.
- Select the appropriate serial port.
- Set the baud rate to **115200**.

## Expected Output
```text
Hello World! imxrt1176_som/mimxrt1176/cm7
```
