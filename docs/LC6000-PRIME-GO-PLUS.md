# LC6000 and generic MIDI on a PRIME GO+ (JP11S)

Investigation date: 2026-11-09. No setting was added. Nothing in this tree
makes an LC6000, or any other USB MIDI controller, drive a PRIME GO+.

This was read from the EngineX tree and from public Denon documents and
Engine OS assignment files. No PRIME GO+, no LC6000, and no JP11S rootfs
were available here. Do not treat any step below as something that has
been tried on a deck.

## Result

An LC6000 (hardware id JC20) is a USB MIDI layer controller. On a unit
Denon supports, it takes layer B, which Engine names deck 3 or deck 4.
The player's own two outputs keep the audio. That matches the shape of
the request, and it is how SC5000 / SC6000 already work.

A PRIME GO+ cannot take that path with the changes EngineX is allowed
to make:

- EngineX on JP11S installs only under `/data`. The signed system
  partition, including `/usr/Engine/Engine` and
  `/usr/Engine/Content/KnownDevices.vfsb`, stays the vendor's.
- The 4-deck unlock in `tools/ui-mod/engine_ui_hook.c` writes
  `numberOfDecks = 4` only when the board is already a player
  (`isPlayer` set). JP11S is a 2-deck console, so the hook leaves the
  deck count alone. The same function folds four rendered decks back
  onto two outputs only for JP07, JP08, JP13 and JP14.
- The stock JC20 assignment binds only when Engine's MIDI thread name
  ends in `3` or `4`. A 2-deck GO+ has no such thread.
- Denon's own LC6000 FAQ lists PRIME 4, PRIME 4+, SC LIVE 4, SC5000 /
  SC5000M and SC6000 / SC6000M. It does not list PRIME GO or PRIME GO+.
  It also says Prime 4 G2 does not support the LC6000, so "second
  generation" and "four decks" are not enough on their own.

A class-compliant map for something like a DDJ-400 has the same block.
Engine will not apply a loose mapping while `KnownDevices.vfsb` still
owns the port, and this tree has no userspace API that can press
`/Engine/DeckN/Play` from outside Engine's QML.

## What the GO+ install can change

`tools/az01-packer/build-data-bundle.py` installs the suite under
`/data/enginex` and, at boot, writes a transient `engine.service`
drop-in. That drop-in sets `LD_PRELOAD` to `libengine-ui-hook.so`. The
hook patches the running Engine process. It does not replace the
binary on disk. The same boot script may bind-mount a file over a
path that already exists (`/etc/sc5000/version.txt`,
`/root/.ssh/authorized_keys`). It cannot create a new directory on the
read-only `/usr`.

The hook's QML replacement walks the Qt resource tree Engine has
already registered. It can swap a file that is already in that tree.
It does not add a new assignment path, and the controller assignments
live on disk under `/usr/Engine/AssignmentFiles/`, not in that tree.

`rootfs-overlay/etc/sc5000/engine-ui-guard` bind-mounts patched
assignment files for JP07, JP08, JP13 and JP14 when 4-deck layout is
on. That script is part of the first-generation image. The JP11S data
bundle does not run it. On a second-generation console the bundle only
records a 4-deck role file, and an unknown role becomes `12`.

There is no MIDI mapper, ALSA sequencer client, or LC6000 setting
anywhere else in this tree. `MidiDeviceScanner` in the 5.0.4 notes is
a small helper that parses `amidi`-style text. It is not a mapper, and
that note is from the SC5000 5.0.4 image, not from a JP11S rootfs.

## How an LC6000 attaches on a supported player

Public assignment files (the Engine OS tree published in
`ghuntley/denon-prime4`):

- `PresetAssignmentFiles/JC20/JC20_Controller_Assignments.qml` is a
  `MidiAssignment` named `LC6000 Controller Assignment`. Transport,
  pads, jog and pitch use the note and CC numbers in Denon's
  *LC6000 PRIME MIDI Specification v1.0*. `DeckAssignmentConfig` sets
  `controllerMode: ControllerMode.Secondary` and
  `name: device.deckName`.
- `JC20_Controller_Device.qml` sets `deckName` from `deviceName`, which
  the comment says is assigned in `airAssignments::Midi::Thread::run()`.
  A name ending in `3` is the Left group. A name ending in `4` is the
  Right group. Anything else is an empty name, so the secondary
  assignment has no deck.
- On completion the device QML sends the Engine OS mode SysEx
  (`F0 00 02 0B 00 10 50 …`) and sets
  `/GUI/Decks/Deck<Left|Right>/ControllerConnected`. The same file says
  Engine OS mode turns Deck Select off, so the LC6000 stays on MIDI
  channel 0 and does not pick an arbitrary deck.

Denon's FAQ says the same thing in product language: in standalone
mode the LC6000 only controls layer B of a dual-layer player. Deck
Select is for computer software. The LC6000 has no audio output. The
jog display is SysEx, not a normal mapped control.

A separate public note (icedream/denon-prime4 discussion 101, and the
"adding slicer" write-up from a Prime GO shell) describes how Engine
chooses the QML:

- `amidi -l` on a Prime GO shows one port, `PRIME GO Control Surface`.
- While `/usr/Engine/Content/KnownDevices.vfsb` is in place, Engine
  uses that blob.
- If the blob is renamed, Engine looks for
  `<ALSA name>_Assignments.qml` and `<ALSA name>_Device.qml`, with
  spaces turned into underscores.
- The product directories under `PresetAssignmentFiles/` (including
  `JC20` and `JP11` on that Prime GO listing) are templates. They are
  not loaded under those filenames for a hot-plugged port.

So a JC20 folder on disk is not evidence that the host will open it
when an LC6000 is plugged in. The blob, and the C++ code that names
the MIDI thread, decide that.

## Why JP11S does not get there

1. **Deck count.** `prepare_config` in `engine_ui_hook.c` runs on the
   shared 5.3.0 getConfig return. When `four_deck_waveform=1` it loads
   `isPlayer`. If that byte is clear, it leaves `numberOfDecks` as the
   model constructor set it. The unit test starts an all-in-one at 2
   decks and expects 2 afterwards, and starts a player at 2 and
   expects 4 with `isPlayer` cleared. JP11S is the all-in-one case.
   Comment at the site: four decks alone crash a player's layout
   controller; Engine offers the 4-deck layout to a non-player that
   already has four decks. A GO+ skin is the 2-deck console layout.
   Forcing `numberOfDecks` to 4 from `/data` is the change this
   investigation was told not to ship, and it is not shipped.

2. **Audio fold.** `four_deck_pad_on` is set only for JP07 / JP08 /
   JP13 / JP14, and the callback copies deck 3 and 4 only when Engine
   already asked for exactly four output channels. JP11 and JP11S are
   now in that test table and must stay off. The GO+ master path is
   the two local decks. USB DAC / USB-B UAC2 output stays the
   experimental API described in the README. The LC6000 does not add
   a sound card, and a DDJ-400's USB audio is not a route to use.

3. **No layer B slot to name.** Without deck 3 and deck 4, Engine has
   nothing to put in the MIDI thread name the JC20 file checks. A
   copied assignment mounted over a missing `JC20` directory cannot
   be attached: a bind-mount needs a mount point that already exists,
   and a copied file with the wrong ALSA name is never opened.

4. **The built-in surface is in the blob.** Hiding
   `KnownDevices.vfsb` from the data-bundle boot script would make
   Engine look for loose QML. The GO+'s own pads, jogs and mixer are
   behind that blob (`PRIME GO Control Surface` on the gen-1 unit).
   A wrong or missing replacement leaves the console's own controls
   dead until the mount is removed. That is not a safe preparatory
   step, and the JP11S blob is not in this repository, so a
   replacement cannot be checked.

5. **Official product list.** The LC6000 FAQ (Denon support article
   69000809994) names the compatible standalone units and stops there.
   PRIME 4+ is included. PRIME GO+ is not. Prime 4 G2, which is a
   four-deck second-generation unit, is called out as unsupported.
   Compatibility is per product image, not a switch in
   `settings.conf`.

6. **The only host port is the drive port.** The PRIME GO+ datasheet
   lists one USB port for drives and one USB-B port "to computer".
   The LC6000 is a USB device (USB-B, bus-powered, optional 12 V
   supply). It can only sit on the drive port. The computer port is
   the GO+ acting as a device, so the two USB-B plugs cannot meet.
   Using the drive port for the controller means the library has to
   come from the SD slot, internal storage, or streaming. Whether
   that USB-A port enumerates the LC6000's MIDI interface under the
   JP11S kernel was not tested.

`player_board()` and the board table in `test_stems_feature.c` keep
JP11S off the scratch-output path so a later edit does not turn the
GO+ into a four-deck player by accident.

## Generic MIDI

A DDJ-400 is class-compliant MIDI as well as a USB sound card. Engine
would see it the same way it sees any port that is not in
`KnownDevices.vfsb`: not at all, until the blob is removed and a pair
of QML files exists whose names match `amidi -l`. Removing the blob
is the hazard in the previous section. The published LC6000 note map
cannot be reused; a DDJ-400 speaks a different map, and its jog
display and HID extras are a separate protocol.

A userspace bridge in the data bundle could read ALSA MIDI without
touching the blob. It would still need a way into Planck. The only
in-tree control path is QML inside Engine (`Planck.getProperty` /
`translator.state`), which the UI packs use for shadow decks. The
existing pack timer that reads settings is a 10 second poll. That
cannot carry a jog or a pitch fader. There is no D-Bus or HTTP
transport for play, cue, or seek in `pkg/webui`. Building one, and a
mapping file format, before a real port has been seen on a GO+ would
be a second controller stack with nothing to test it against.

## What a later change has to show first

On a GO+ that already runs this data bundle, with the LC6000 on its
own power supply and plugged into the USB-A drive port:

1. `amidi -l` (or the kernel log) shows a MIDI port, and the GO+'s
   own controls still work. Record the exact ALSA name.
2. The directory listing of
   `/usr/Engine/AssignmentFiles/PresetAssignmentFiles/` and the
   presence of `/usr/Engine/Content/KnownDevices.vfsb`. Do not rename
   the blob.
3. A sandbox boot of that rootfs (`unshare` plus the unpacked image,
   not a Factory Reset on the deck) after any proposed `numberOfDecks`
   edit, to see whether the GO+ layout controller survives deck 3
   and deck 4. The current hook test is the reason not to skip this:
   the all-in-one case is specified to stay at 2.

Until those three exist, there is no toggle to turn on. Settings →
Device → 4-Deck Role on a GO+ is the LAN shadow-deck link (role `12`
by default on a second-generation console). It does not attach a USB
controller, and decks 3 and 4 on a console stay local unless a
network peer has loaded them.

## Risks if those checks are skipped

- Writing `numberOfDecks = 4` on JP11S can crash the layout
  controller the same way the hook comment describes for a player,
  or it can leave two extra silent decks that the 2-channel mixer
  never sums. Either way the audience path is the GO+'s own outputs.
- Bind-mounting over `KnownDevices.vfsb` can disable the built-in
  surface. Recovery is a reboot after the mount is gone; there is
  no root SSH on this unit to fix a bad mount from the outside.
- Opening the LC6000's MIDI port from a helper steals it from Engine
  if Engine ever does claim it.
- The LC6000 draws bus power. The FAQ says to use the included supply
  when the host port cannot feed it. A brown-out on the only USB
  host port also drops the drive.
