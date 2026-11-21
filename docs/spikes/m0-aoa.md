# M0 spike: Android Open Accessory (AOA) on the target tablet

Date: 2026-11-21. Host: NixOS 26.11, kernel 6.18.63, libusb 1.0.30. Device: Samsung tablet, USB id `04e8:6860`
(MTP + CDC ACM + ADB composite), Android 16, ADB/developer mode enabled. Throwaway code: `tools/spikes/aoa_spike.c`.

## Verdict

**AOA works** on this device, from an unprivileged user process. No app is needed on the device for the switch itself.

## What was run

    ./aoa_spike 04e8     # libusb: GET_PROTOCOL (51), SEND_STRING x6 (52), START (53), wait for re-enumeration

Results:

- `GET_PROTOCOL` returned **version 2**.
- `START` returned ok. The device left the bus and came back as **`18d1:2d01`** ("Android Open Accessory device
  (accessory + ADB)") after **~800 ms**. The `+ ADB` is because ADB is enabled on this tablet; with ADB off it
  would be `2d00` (not tested).
- Endpoints in accessory mode: interface 0 (class ff) with bulk `0x81` IN and `0x01` OUT, both max packet 512
  (accessory pair), plus interface 1 (class ff, sub 0x42, ADB) with bulk `0x82`/`0x02`. Link speed is USB 2.0 high
  speed (480 Mbit/s); the link is `/sys/bus/usb/devices/1-3`, version 2.00.
- Both the original (`04e8:6860`) and the accessory-mode (`18d1:2d01`) device nodes got a `user:rand:rw-` ACL
  from the seat's uaccess handling, so libusb could open them as a normal user with no custom udev rule.
  (Not guaranteed on other distros, so M7's udev rule is still needed.)

## Consequences

- Bandwidth: USB 2.0 bulk is ~35 to 40 MB/s in practice. The HEVC stream measured in the encode spike
  (~19 to 30 Mbit/s) is under 10% of that, so USB 2.0 is enough. Static refinement frames and 10-bit will be
  larger but still small.
- While in accessory mode the tablet no longer exposes MTP; replugging (or ending the accessory session) restores it.
- A tablet screen prompt or app auto-launch needs an Android app with an accessory intent filter and an
  accessory-filter XML matching the strings sent. That is M7 and cannot be judged without the app.

## Not verified

- Behaviour with ADB off (`2d00`), and whether Android 16 / One UI shows the "allow access" prompt as in the plan.
  Needs the Android app.
- Real bulk throughput and latency between this host and the tablet (needs the app on the other end).
- Re-attach behaviour: a second `START` while already in accessory mode, unplug/replug timing, sleep/resume.
