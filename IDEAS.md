# Dilly-dally

![Dilly-dally idling on a desk](images/face-calm.jpg)

**A desk companion that refuses to sit still.**

A face that blinks, drifts and sulks. A seven segment clock. A focus timer that
watches you work. Internet radio, old speeches, and Bluetooth audio out to
whatever is in your ears. Core board only, nothing to attach.

## Five pages

| Page | What lives there |
| --- | --- |
| **Face** | A glowing orb with a mood. Blinks on its own clock, bobs on another, reacts to touch |
| **Clock** | Seven segment time, day, date, and a small face keeping you company |
| **Focus** | Pick 15, 25, 45 or 60 minutes or step your own. Start, pause, stop. A chime at the end |
| **Listen** | Nine radio stations and five public domain speeches, streamed |
| **Settings** | Wi-Fi, Bluetooth audio, time zone, auto page change, 24 hour clock, brightness, volume, chime |

Swipe between them, or let face and clock take turns on their own.

![Seven segment clock](images/clock.jpg)

## The face

Ten moods, sixty frames, all held decoded so a change never waits. Poke it and
it lights up. Poke it four times and it blushes. Leave it alone and it
daydreams. Run a focus session and it gets anxious in the last minute, worried
if you pause too long, cross if you quit, and celebrates when you finish.

![A happy face after a poke](images/face-sunny.jpg)

## First boot

Nothing secret is compiled in. Settings, then Wi-Fi. The board raises its own
network and shows the name and password on screen. Join it, open
`192.168.4.1`, pick your network. The clock goes live as soon as time syncs.

Or over ESP-Iris RPC.

```powershell
.\m.ps1 iris rpc 20482 1 --payload "ssid=YOUR_WIFI_NAME"
.\m.ps1 iris rpc 20482 1 --payload "pass=YOUR_WIFI_PASSWORD"
```

## Bluetooth audio

Settings, then Bluetooth audio. Put earbuds or a speaker in pairing mode and
tap it in the list. Radio, speeches and the chime all go to it from then on,
and it reconnects at boot. Classic Bluetooth A2DP, so AirPods and nearly every
speaker work.

![Settings page](images/settings.jpg)

## Source

Full source, the face renderer, the simulator and a page by page walkthrough
test are on GitHub. Build photos go on Instagram as
[@hackhobby_lab](https://www.instagram.com/hackhobby_lab/).

Apache-2.0. DSEG7 and DejaVu fonts keep their own licences.
