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
| `PS3_NP_ONLINE_ID=name` | The player's name. It also gives the instance its own console identity: the OpenPSID (`sys_ss_get_open_psid`) and MAC are derived from it. Titles tell consoles apart by these, so two instances with the same identity count as one player. Without it, both stay at their old fixed values, so offline saves are untouched. |
| `PS3_OPEN_PSID=<32 hex>` | Set the OpenPSID outright instead. |
| `PS3_NET_P2P_PORT=n` | The host port this instance's P2P sockets bind, whatever port the title asks for. Default 3658. psnr hands this port to peers. Two instances on one machine need two ports. |
| `PS3_NP_SIGNALING_DELAY_MS=n` | How long after a member joins that signaling reports them "established". Default 1000. |
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
- the host creates a match and the other player finds it with Quick Match;
- both appear in each other's online lobby and pick characters;
- the host starts the game;
- the room goes through psnr, and lobby traffic runs peer to peer over UDP
  and TCP P2P sockets.

The match does not start yet. The title sends its game setup as a zlib stream
over TCP, and the host's compressor emits a 10-byte "stream" whose bytes change
from run to run for a 58 KB setup. The receiver drops it, and both sides sit on
"TRANSFERRING GAME SETUP". The compressor is the title's own statically linked
zlib, running as lifted code, so this is a recompiler or runtime correctness
problem, not a networking one.

Offline, with none of the variables set, Simpsons boots, reports NP offline,
shows its "sign in to post scores" notice, and plays Stage 1.

Tests: `libs/network/tests/test_np_matching2.c` covers response relocation,
attribute packing and the room structures a title reads. `test_sys_net.c`
covers sockets over loopback, including a P2P bind and receive.

What it took to get the lobby up, each of which a title will hit:
- **RoomMemberDataInternal layout.** userInfo is at +4 and memberId at +56,
  after the NP ID.
- **Signaling timing.** "Established" is deferred until the host has handled
  the join.
- **Update events for the writer.** The member who writes room internal data
  receives the update event too.
- **Distinct console identity.** Each player needs their own OpenPSID; the
  title hashes it into the key each player announces.
- **P2P sockets.** They bind all interfaces. Stream sockets connect to the
  vport, and spare binds go to ephemeral ports.
- **libnet errors.** A failing call returns `0x80010200 | errno` in-band.

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
