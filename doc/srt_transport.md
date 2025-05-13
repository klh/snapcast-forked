# SRT Transport in Snapcast

## Overview

Snapcast now supports SRT (Secure Reliable Transport) as a transport protocol for audio streaming. SRT is designed to deliver high-quality, low-latency video and audio over unpredictable networks like the Internet.

## Benefits of SRT

- **Improved reliability on lossy networks**: SRT includes packet loss recovery mechanisms
- **Lower latency with configurable trade-offs**: Adjust latency to balance between reliability and delay
- **Dynamic adaptation to network conditions**: SRT automatically adjusts to changing network conditions
- **Built-in encryption**: Optional encryption for secure streaming
- **Better performance on Wi-Fi and Internet**: Optimized for challenging network environments
- **Bandwidth efficiency**: Optimized bandwidth usage compared to TCP-based protocols

## Configuration

### Server Configuration

SRT can be configured in the server configuration file (`snapserver.conf`) or via command-line options:

```ini
[srt]
# enable SRT audio streaming
# set to false if you want to disable SRT and use only TCP or WebSocket
enabled = true

# which port the server should listen on for SRT connections
# this is different from the standard TCP port (1704) to avoid conflicts
port = 1706

# address to listen on, can be specified multiple times
# use "0.0.0.0" to bind to any IPv4 address or :: to bind to any IPv6 address
bind_to_address = ::

# latency in milliseconds
# higher values provide better reliability on lossy networks
# but increase overall latency
# recommended values:
# - 20-40ms for local networks with minimal packet loss
# - 60-100ms for typical home Wi-Fi networks
# - 100-200ms for connections over the internet or unstable networks
latency = 120

# enable encryption for SRT transport
# when enabled, all SRT traffic will be encrypted using AES-128/256
# this adds security but has minimal performance impact
encryption = false

# encryption passphrase
# only used if encryption is enabled
# should be a strong password or passphrase
# minimum recommended length is 16 characters
passphrase = 

# maximum bandwidth in bytes per second (0 = unlimited)
# useful for limiting bandwidth usage on constrained networks
# example: 256000 would limit to ~2Mbps
max_bandwidth = 0
```

### Client Configuration

SRT can be configured on the client side using command-line options:

```bash
snapclient --srt-latency=120 --srt-encryption --srt-passphrase="your_secure_passphrase" srt://server_ip:1706
```

Client configuration options explained:

- `--host=<server>`: Server hostname or IP address (deprecated, use the URL format instead)
- `--port=<port>`: Server port (deprecated, use the URL format instead)
- `--server=<server:port>`: Server with port (deprecated, use the URL format instead)
- `srt://server_ip:port`: Connect using SRT protocol (preferred format)
- `--srt-latency=<ms>`: Set the SRT latency in milliseconds (default: 120)
- `--srt-encryption`: Enable SRT encryption (must match server setting)
- `--srt-passphrase=<pass>`: Set encryption passphrase (required if encryption is enabled)
- `--on-server`: Indicates that the client is running on the same machine as the server
  - When this flag is set, the client will:
  - Skip chrony time synchronization setup
  - Use local monotonic clock for timing
  - Avoid unnecessary network operations

## Protocol Selection

Snapcast will use SRT by default when available, but will fall back to WebSocket if SRT fails or is not available. To explicitly select a protocol, use the URL scheme in the client:

- `srt://server_ip:1706` - Use SRT protocol
- `tcp://server_ip:1704` - Use TCP protocol
- `ws://server_ip:1780` - Use WebSocket protocol
- `wss://server_ip:1788` - Use secure WebSocket protocol

## Dependencies

To use SRT transport, you need to install the SRT library:

### Debian/Ubuntu
```bash
sudo apt install libsrt-openssl-dev srt-tools
```

### Arch Linux
```bash
sudo pacman -S srt
```

### Fedora
```bash
sudo dnf install srt-devel srt-tools
```

### macOS
```bash
brew install srt
```

## Building with SRT Support

When building Snapcast from source, enable SRT support with:

```bash
cmake -DBUILD_WITH_SRT=ON ..
make
```

## Troubleshooting

If you encounter issues with SRT:

1. Ensure both server and client have SRT support enabled
2. Check that the SRT port (default: 1706) is open in any firewalls
3. Try increasing the latency value for more reliable transmission on unstable networks
4. If using encryption, ensure the passphrase matches on both server and client
5. Check server logs for SRT-related messages

## Technical Details

SRT operates on UDP and provides:

- Connection-oriented transmission like TCP
- Reliable data delivery with packet retransmission
- Configurable latency buffer for handling jitter
- Timestamp-based packet delivery
- Bandwidth estimation and congestion control
- Optional AES encryption

The default latency of 120ms provides a good balance between reliability and delay for most home networks. For very stable networks, you can reduce this value, while for unstable networks (like Wi-Fi or Internet), you might want to increase it.
