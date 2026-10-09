```
 _____  _   _  _____  _  _  _     
|_   _|| | | |/  ___|| |(_)| |     Steam    
  | |  | |_| |\ `--. | | _ | |__     In-Home
  | |  |  _  | `--. \| || || '_ \      Streaming
 _| |_ | | | |/\__/ /| || || |_) |       Library
 \___/ \_| |_/\____/ |_||_||_.__/ 
```

IHSlib is a library to do discovery, authorization and streaming
for [Steam Remote Play](https://store.steampowered.com/remoteplay) protocol.

Based on [reverse engineering work](https://github.com/mariotaku/steamlink.py) of Steam Link hardware, its goal is to
be core of fully featured Steam Remote Play client.

This project is licensed under LGPL v3.

## Credits

[SteamDatabase/Protobufs](https://github.com/SteamDatabase/Protobufs) - Up-to-date protobufs thanks to this project!

## This fork's compatibility contract

`kxn/ihslib` maintains `master` for nsteamlink and pulls official
`mariotaku/ihslib` changes into that branch. The official master at `1881b9a`
is included in its history. Upstream changes are merged with the fork's
protocol definitions; generated protobuf sources use protobuf-c 1.5.2.

This fork requires applications and the library to be rebuilt together. It is
not binary ABI compatible with the official release: public configuration and
callback structures grew, and the legacy video submit callback includes a
`uint16_t frameId`. Existing fork callbacks remain supported. New platform
capabilities use a separate `IHS_StreamClientCapabilities` structure rather
than extending `IHS_SessionConfig` again.

Call `IHS_SessionSetClientCapabilities()` before connecting or creating data
channels to describe the actual platform and decoder. The session copies both
strings and keeps them until destruction. Fields left unknown are omitted;
zero capability bitrate limits do not impose a Switch-specific limit.
Requested encode resolution, quality and bitrate still use `IHS_SessionConfig`.
The library's defaults do not claim Switch or Marvell hardware, TV form factor,
or knowledge of suspend support. Decoder-reported loss flushes assembly state
and waits for a keyframe in both legacy and tracked callback modes. Tracked
callbacks retain their existing ticket ownership/completion requirements.

Authorization and streaming retain distinct PIN semantics. After successful
pairing, retrieve the current secret with `IHS_ClientGetSecretKey()`, persist
it for that host, and select it with `IHS_ClientSetSecretKey()` before streaming.
Legacy KeyEscrow hosts use the installation secret. Secret rotation is not
implemented; proof responses do not claim to have stored an updated secret.

The SDL HID provider collects canonical input state on the SDL owner thread;
applications flush collected changes explicitly and process pending SDL writes
on that thread. Disconnect closes transport; `IHS_SessionStopGame()` is the
separate explicit host-game stop action.
