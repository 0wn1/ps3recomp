# Online play (psnr)

Titles that matchmake through sceNpMatching2 find each other through a
[psnr](https://github.com/sp00nznet/psnr) server. psnr is a small stand-in for
PSN's rooms and leaderboards. Once players are matched, the title's own
traffic goes directly between them over `sys_net` P2P sockets; the server does
not carry it.

Offline is still the default. Nothing below changes a title unless these
variables are set.

| Variable | What it does |
|---|---|
| `PS3_NET_ONLINE=1` | Real host sockets (`libs/network/sysNet.c`), and cellNetCtl reports a connection. |
| `PSNR_SERVER=host[:port]` | The psnr server. Port 36100 by default. With `PS3_NET_ONLINE`, the NP manager reports ONLINE. |
| `PS3_NP_ONLINE_ID=name` | The player's name. Two instances need two names. |
| `PS3_NET_P2P_PORT=n` | The host port this instance's P2P sockets bind, whatever port the title asks for. Default 3658. psnr hands this port to peers. Two instances on one machine need two ports. |
| `PS3_NET_TRACE=1` | Log every packet sent, and every packet received (not the empty polls). |

## Two players on one machine

```
psnr                                    # server/ in the psnr repo; listens on :36100
PS3_NET_ONLINE=1 PSNR_SERVER=127.0.0.1 PS3_NP_ONLINE_ID=homer PS3_NET_P2P_PORT=3658 ./build/simpsons ...
PS3_NET_ONLINE=1 PSNR_SERVER=127.0.0.1 PS3_NP_ONLINE_ID=bart  PS3_NET_P2P_PORT=3659 ./build/simpsons ...
```

## What is implemented

| Module | State |
|---|---|
| sceNpMatching2 | Contexts, one server and one world (both answered locally), and rooms on psnr: search, create/join, leave, kick, room data, room messages. Room, room-message and signaling callbacks are delivered from `cellSysutilCheckCallback`. Signaling reports a member as established as soon as psnr names them. |
| sceNpScore | Title and transaction contexts. Record score, and ranking by range and by NP ID, sync and async, all on psnr leaderboards. |
| sceNpLookup | Profiles are answered locally from the online ID. There is no avatar. |
| cellSysutilAvc2 | Voice chat loads, joins and streams, but carries no audio. |
| sceNp manager | Reports ONLINE when psnr is set, and fires the manager callback once. |
| sys_net | P2P sockets map to plain UDP/TCP on `PS3_NET_P2P_PORT`. There is no vport multiplexing. |

## How it was verified

Simpsons Arcade (NPUB30563) was run as two instances on one machine:
- one instance creates a match and the other finds it with Quick Match;
- both see each other in the room;
- signaling hands each side the other's address and P2P port.

The title's own session does not start yet. Neither side sends its first P2P
packet, and the joining player gives up after about five seconds ("Session is
no longer available"). The title's transport assigns member IDs to its
channels from its own room bookkeeping, and that path is still being traced.

Tests: `libs/network/tests/test_np_matching2.c` covers response relocation,
attribute packing and the room structures a title reads. `test_sys_net.c`
covers sockets over loopback.

## Why it is shaped this way

- **One thread.** Callbacks run on the title's polling thread, not on a
  Matching2 worker. psnr replies are pumped from `cellSysutilCheckCallback`,
  so every callback runs where titles already expect sysutil callbacks.
  Score's `PollAsync` pumps too, for titles that poll from a worker.
- **Responses are relocatable images.** An SDK response contains pointers into
  itself, and those only mean something once the title picks the buffer in
  `GetEventData`. Each response is built with its pointer fields recorded, then
  patched as it is copied into the title's buffer.
- **Attributes are opaque to the server.** Matching2 bin and int attributes are
  packed into blobs by the client. psnr stores and forwards bytes, and search
  filtering happens client-side on the rooms psnr lists.
