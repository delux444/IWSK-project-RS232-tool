# SerialCtrl — Serial Port Communication Tool

A Linux command-line application written in C for RS-232/UART serial port communication. Supports full port configuration, text and binary transmission modes, link testing (PING), transactions with timeout, and manual modem line control.

---

## What is RS-232 and How Does It Work?

**RS-232** (*Recommended Standard 232*) is one of the oldest and most widely used serial communication standards, developed in 1960 by the EIA organization. Despite its age, it remains common in industrial automation, embedded systems, and test equipment.

### How It Works

Serial communication transmits bits **one at a time** over a single data line. In RS-232, logic states are represented as voltages:

| Logic State | Voltage (TxD/RxD) |
|-------------|-------------------|
| `1` (MARK)  | −3 V to −15 V     |
| `0` (SPACE) | +3 V to +15 V     |

Each data frame consists of:
- **1 start bit** (always `0`)
- **5–8 data bits**
- **optional parity bit** (Even / Odd / None)
- **1 or 2 stop bits** (always `1`)

### RS-232 Pin Layout (DB9 Connector)

| Pin | Name | Direction | Description          |
|-----|------|-----------|----------------------|
| 1   | DCD  | ←         | Data Carrier Detect  |
| 2   | RxD  | ←         | Receive Data         |
| 3   | TxD  | →         | Transmit Data        |
| 4   | DTR  | →         | Data Terminal Ready  |
| 5   | GND  | —         | Signal Ground        |
| 6   | DSR  | ←         | Data Set Ready       |
| 7   | RTS  | →         | Request To Send      |
| 8   | CTS  | ←         | Clear To Send        |
| 9   | RI   | ←         | Ring Indicator       |

### Flow Control

When a transmitter is faster than the receiver, flow control prevents data loss:

| Method       | Type     | Description                                        |
|--------------|----------|----------------------------------------------------|
| **None**     | —        | Data sent without synchronization                  |
| **RTS/CTS**  | Hardware | Transmitter waits for CTS signal from the receiver |
| **DTR/DSR**  | Hardware | Terminal signals readiness via DTR                 |
| **XON/XOFF** | Software | Receiver sends `0x11` (XON) / `0x13` (XOFF) bytes |

### Null-Modem Cable (Direct DTE↔DTE Connection)

To connect two computers directly without a modem, a crossover (null-modem) cable is used:

```
DTE A          DTE B
TxD  ────────►  RxD
RxD  ◄────────  TxD
DTR  ────────►  DSR
DSR  ◄────────  DTR
RTS  ────────►  CTS
CTS  ◄────────  RTS
GND  ───────── GND
```

Connectors: **DB9F on both ends.**

---

## Features

### Mandatory Project Features (OB)

| Feature | Description |
|---------|-------------|
| **Port selection** | Auto-detection and listing of available ports (`/dev/ttyUSB*`, `/dev/ttyACM*`, `/dev/ttyS*`) |
| **Transmission parameters** | Baud rate 150–115200 bit/s; data bits: 7 or 8; parity: E/O/N; stop bits: 1 or 2 |
| **Flow control** | None, RTS/CTS (hardware), XON/XOFF (software), DTR/DSR (hardware) |
| **Terminator** | None, CR, LF, CR+LF, or custom 1–2 character sequence |
| **Transmit** | Send text messages with configurable terminator |
| **Receive** | Continuous receive loop with terminal output |
| **PING** | Link integrity test with round-trip delay measurement [ms] |
| **Text TX/RX mode** | Interactive simultaneous send and receive using background thread |

### Optional Project Features (OP)

| Feature | Description |
|---------|-------------|
| **Transaction** | Send a message and wait for a response with a configurable timeout |
| **Manual DTR/RTS control** | Set or clear DTR/RTS lines on demand; monitor DSR and CTS input states |
| **Binary (hex) mode** | Interactive hex editor for sending arbitrary bytes; received data displayed in hex |

---

## Installation

### Requirements

- Linux (tested on Debian 13)
- GCC compiler
- POSIX threads (`pthreads`) — included in standard `libc`
- Access to a serial port device (you may need to add your user to the `dialout` group)

### 1. Clone the Repository

```bash
git clone https://github.com/your-username/serialctrl.git
```
```bash
cd serialctrl
```

### 2. Add User to `dialout` Group (if needed)

Without this step, opening `/dev/ttyUSB*` or `/dev/ttyACM*` may return a permission error:

```bash
sudo usermod -aG dialout $USER
```
Log out and back in for the change to take effect

### 3. Compile

```bash
gcc -o serialctrl serialctrl.c -lpthread
```

### 4. (Optional) Install System-wide

```bash
sudo cp serialctrl /usr/local/bin/
```

After this you can run `serialctrl` from anywhere without `./`.

### 5. Verify

- show help
```bash
./serialctrl -h
```
- list available serial ports
```bash
./serialctrl -l
```

---

## Usage

```
serialctrl -d <port> [options]
```

### Configuration Options

| Option | Description | Default |
|--------|-------------|---------|
| `-d <port>` | Serial device, e.g. `/dev/ttyUSB0` | *(required)* |
| `-b <baud>` | Baud rate: `150 300 600 1200 2400 4800 9600 19200 38400 57600 115200` | `9600` |
| `-s <bits>` | Data bits: `7` or `8` | `8` |
| `-p <par>` | Parity: `N` (none), `E` (even), `O` (odd) | `N` |
| `-S <stop>` | Stop bits: `1` or `2` | `1` |
| `-f <flow>` | Flow control: `0`=none, `1`=RTS/CTS, `2`=XON/XOFF, `3`=DTR/DSR | `0` |
| `-t <term>` | Terminator: `CR`, `LF`, `CRLF`, custom 1–2 char, or `none` | `none` |
| `-m <msg>` | Message to send (text or hex string in binary mode) | — |
| `-l` | List available serial ports and exit | — |
| `-h` | Show help and exit | — |

### Operating Modes

| Option | Mode | Description |
|--------|------|-------------|
| *(default)* | **Interactive TX/RX** | Simultaneous send and receive in text mode |
| `--listen` | **Listen only** | Receive only, no transmit |
| `--ping` | **PING** | Link test with round-trip delay measurement |
| `--binary` | **Binary** | Hex editor mode; TX and RX displayed in hexadecimal |
| `--transaction` | **Transaction** | Send message and wait for response with timeout |

### Additional Options

| Option | Description |
|--------|-------------|
| `--timeout <ms>` | Transaction response timeout in milliseconds (default: `2000`) |
| `--set-dtr <0\|1>` | Set (`1`) or clear (`0`) the DTR line |
| `--set-rts <0\|1>` | Set (`1`) or clear (`0`) the RTS line |
| `--monitor` | Display current state of modem control lines (DTR, RTS, DSR, CTS) |

### Examples

- List all detected serial ports
```bash
./serialctrl -l
```

- Interactive TX/RX at 115200 baud with CR+LF terminator
```bash
./serialctrl -d /dev/ttyUSB0 -b 115200 -t CRLF
```

- Send a one-shot message and exit
```bash
./serialctrl -d /dev/ttyUSB0 -b 9600 -m "Hello" -t LF
```

- PING test with LF terminator
```bash
./serialctrl -d /dev/ttyUSB0 --ping -t LF
```

- Transaction with 500 ms timeout
```bash
./serialctrl -d /dev/ttyUSB0 -m "STATUS?" -t CRLF --transaction --timeout 500
```

- Binary mode — send bytes 0x01 0x02 0x03
```bash
./serialctrl -d /dev/ttyUSB0 --binary -m "010203"
```

- Listen-only mode at 19200 baud
```bash
./serialctrl -d /dev/ttyUSB0 --listen -b 19200
```

- Hardware RTS/CTS flow control
```bash
./serialctrl -d /dev/ttyUSB0 -b 9600 -f 1
```

- Set DTR high and display modem line status
```bash
./serialctrl -d /dev/ttyUSB0 --set-dtr 1 --monitor
```

- 7 data bits, even parity, 2 stop bits at 4800 baud
```bash
./serialctrl -d /dev/ttyUSB0 -s 7 -p E -S 2 -b 4800
```
---

> Course project — *Interfaces in Computer Systems*, Academic Year 2025/2026
