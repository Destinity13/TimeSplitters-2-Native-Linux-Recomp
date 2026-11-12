<sub>[← back to README](../README.md) · [how online play works](../docs/ONLINE.md)</sub>

# TS2 server: host your own online server

> [!NOTE]
> Online play is in the packages since **1.0.0**; `ts2-server` is in every package (`ts2-server` on Linux, `ts2-server.exe` on
> Windows). Current state: [docs/ONLINE.md](../docs/ONLINE.md).

`ts2-server` is a small always-on relay that turns the internet into one big LAN for TimeSplitters 2. Everyone on the
same server sees each other's games under **Arcade → Network → Join Game**, exactly like PS2s on one network
switch. Game modes, maps, bots and up to 16 players work as in the original LAN mode.

- one C++ file, no dependencies, ~60 KB binary
- one port for TCP and UDP (default **27713**)
- runs on Linux (a VPS, a Raspberry Pi, a home server, a Proxmox LXC container) and on Windows

## Build

```sh
g++ -O2 -std=c++17 -o ts2-server ts2_server.cpp                      # Linux
x86_64-w64-mingw32-g++ -O2 -std=c++17 -o ts2-server.exe ts2_server.cpp -lws2_32   # Windows (or zig c++ -target x86_64-windows-gnu)
```

## Run

```sh
./ts2-server --name "My TS2 Server" --max 16 --motd "Have fun!"
```

| Option | Default | Meaning |
|---|---|---|
| `--port` | `27713` | TCP **and** UDP port |
| `--name` | `TS2 Server` | name shown in the server browser |
| `--max` | `16` | maximum number of players |
| `--motd` | – | message shown to players when they join |
| `--password` | – | private server: players need this password |

Open the port for **TCP and UDP** in your router/firewall. Players enter your address (`host` or `host:port`) in the
game's settings menu (ESC/F1 → Online).

## As a service (systemd)

A hardened unit is in [ts2-server.service](ts2-server.service) (no privileges, read-only filesystem, no home, syscall
filter, memory and task limits):

```sh
sudo mkdir -p /opt/ts2-server && sudo cp ts2-server /opt/ts2-server/
sudo cp ts2-server.service /etc/systemd/system/   # edit --name / --password / --max in ExecStart
sudo systemctl enable --now ts2-server
journalctl -u ts2-server -f      # who joins and leaves
```

## What the server sees

Only the game's LAN traffic between the players and their names. Each player gets a virtual address `10.13.x.y`;
real IP addresses are never passed to other players. Nothing is stored on disk.

## Security model

The server is a **relay**: it never parses game data, it only forwards frames between sessions. The attack surface is
the small control protocol (length-prefixed frames, every field bounds-checked, 1 MiB frame cap, 8 MiB output cap per
client) and the UDP path (session token checked on every datagram; the source address is stamped by the server, so a
player cannot spoof another player's virtual address).

Built-in limits against abuse:

| | |
|---|---|
| session token | 32 bit from the OS random source (`/dev/urandom`, `rand_s`) |
| connections | 64 in total, 4 per source address; unauthenticated ones time out after 10 s, idle players after 30 s |
| forwarding | 600 packets/s and 1 MiB/s per player; a sustained flood disconnects the player (normal play is a few dozen packets/s) |
| TCP streams | 32 per player |
| server-list query (`T2IN`) | the reply is larger than the request, so replies are capped at 40/s to keep the server useless as a reflector |
| password | plain text inside the TCP session (a lobby password, not a secret) |

The game itself talks only to the server, never directly to other players, so nobody learns your IP address. The
game's own network code runs inside the port's 32 MB PS2 memory sandbox: anything a malicious peer could do with
malformed game packets stays inside that sandbox (it can crash *your* game, not run code on your PC). Run the server
under the unit above and it has no privileges to lose.
