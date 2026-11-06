# Super Mario All-Stars (USA): sound system, music and SFX

This document covers the sound system for the All-Stars menu, SMB1, Lost Levels (LL), SMB2 and SMB3. It was written for the PC port's **Music Player** and **Sound Select**.

How each claim was checked:

- **S (scene):** the ID was captured with `ports`/`watch` while the scene played.
- **C (code):** the ID comes from disassembly of the code that requests it.
- **M (melody):** the song data was decoded from ARAM into notes.
- **A (audio):** the ID was played in isolation and the peak/RMS was measured.

All addresses are hex. `$7Exxxx` is WRAM. Unless marked otherwise, `$xxxx` inside the APU means ARAM.

---

## 0. Summary

* **The same SPC700 driver runs in all five titles.** It is Nintendo's N-SPC, and the ID string `Ver S1.20` is at ARAM `$0FC8`. The code (ARAM `$0500-$260E`), the SFX data, the instrument tables (`$3D00`, `$3EE8`) and the sample bank (`$3C00` directory, `$4000-$BC1F` BRR data) are **byte-identical** in the menu, SMB1, LL, SMB2 and SMB3. A title differs only in its **song block, uploaded to `$C000`**.
* **Port roles are the same in every title:**

  | Port | Role |
  |---|---|
  | `$2140` (port 0) | SFX channel A |
  | `$2141` (port 1) | SFX channel B: jump, skid, looping wind, plus control codes `$FF` and `$F0` |
  | `$2142` (port 2) | **music** |
  | `$2143` (port 3) | SFX channel C |

* **Commands are edge-triggered.** The driver acts when a port value changes. It needs no counter and no handshake. The APU echoes the command on the same output port, and the CPU uses that echo as an acknowledgement before it writes 0.
* **Game-side queues.** Write a value here and the NMI sender sends it on the next frame:

  | Title | port0 | port1 | port2 (music) | port3 | Sender |
  |---|---|---|---|---|---|
  | Menu | `$7E0060` | `$7E0061` | `$7E0062` | `$7E0063` | `$00:87CC` (JSR from `$00:8320`) |
  | SMB1 and LL (shared) | `$7E1600` | `$7E1601` | `$7E1602` | `$7E1603` | `$04:8163` (JSL from `$03:81DB` SMB1 NMI, `$0D:8093` LL NMI) |
  | SMB2 | `$7E1DE0` | `$7E1DE1` | `$7E1DE2` | `$7E1DE3` | `$11:95E4` (JSR from `$11:931B`) |
  | SMB3 | `$7E1200` | `$7E1201` | `$7E1202` | `$7E1203` | `$22:E677` (JSL from `$20:F0C7`) |

* **Music command values** are the same everywhere:

  | Value | Effect |
  |---|---|
  | `$01..$18` | play song N from the current bank |
  | `$19..$7F` | ignored |
  | `$F0` | stop now |
  | `$F1` | music volume down (pause) |
  | `$F2` | volume back up (unpause) |
  | `$F3` | fade out over about 1 s, then stop |
  | `$80..$EF`, `$F4..$FF` | fade out and stop, like `$F3` |
  | `$00` | nothing |

* **Music runs with only the APU emulated.** This was verified. The music plays correctly from a save state's APU, and also from an ARAM image built only from ROM data (section 5).
* **SMB3 has two song banks.** The **level/title bank** holds 24 songs; the **world-map bank** holds 16 songs, including toad houses and spade games. The game uploads the right bank at scene changes, so an ID means different songs in each bank.

---

## 1. ARAM layout and driver

| ARAM | Contents | Same in all titles? |
|---|---|---|
| `$0000-$00DF`, `$0100-$01FF`, `$0200-$03FF` | driver variables, stack, voice tables (cleared by driver init) | runtime |
| `$0500-$260E` | driver code, command tables, **all SFX data** | yes |
| `$2C00-$3BFF` | echo buffer (DSP ESA=`$2C`, EDL=2) | runtime |
| `$3C00-$3C6F` | sample directory (28 samples) | yes, after the title-screen upload |
| `$3D00-$3D9B` | instrument table | yes |
| `$3EE8-$3EFF` | note duration and velocity tables | yes |
| `$4000-$BC1F` | BRR samples | yes, after the title-screen upload |
| `$C000-...` | **song block of the current title/bank.** The table at `$C000` is indexed by ID; entry ID is at `$C000+2*(ID-1)` | per title |

**Driver main loop, `$0547`.** Timer 0 ticks every 2 ms (`$FA=$10`).

- On each SFX tick it reads port 0 into `$00`, port 1 into `$01` (and echoes `$05|$26` on out1), and port 3 into `$03`.
- On each music tick (tempo-driven) it reads port 2 into `$02`.
- The read routine is `$05ED`. It reads the port twice until the value is stable. It stores the value only if it differs from the last value read (`$08+X`), and otherwise stores 0. This is the edge detection.
- Before reading, it writes `$D2+X` (the echo value) to the output port.

**Song format.** Standard N-SPC:

- The song pointer points to a list of 16-bit phrase pointers. `$0000` ends the list. `$00FF,addr` loops.
- Each phrase is 8 track pointers.
- Track bytes: `$01-$7F` duration (plus an optional quality byte), `$80-$C7` notes, `$C8` tie, `$C9` rest, `$CA-$DF` percussion, `$E0-$FA` commands.
- Command lengths are at `$0D0D`: `E0:1 E1:1 E2:2 E3:3 E4:0 E5:1 E6:2 E7:1 E8:2 E9:1 EA:1 EB:3 EC:0 ED:1 EE:2 EF:3 F0:1 F1:3 F2:3 F3:0 F4:1 F5:3 F6:0 F7:3 F8:3 F9:3 FA:1`.
- The dispatch table is `$0C17+2*cmd`.

---

## 2. Protocol

### 2.1 How the driver handles each port

**Port 2, music.** The handler is `$08BB`, then `$07E7`.

| Value | Driver action |
|---|---|
| `$01-$18` | Starts song N. Pointer = word at `$BFFE+2N`. A zero high byte means an empty entry: nothing plays. The limit check is `CMP #$19` at `$07F9`. |
| `$19-$7F` | Ignored, but out2 still echoes the value. |
| `$F0` | Stop. All music voices key off. `$06`, `$D4` and `$47` are cleared. |
| `$F1` | Saves the master volume `$59` into `$03F1` and sets `$59=$70`: music goes to about 1/3. Verified A: RMS 1576 → 574. |
| `$F2` | Restores `$59` from `$03F1`. Verified A: RMS back to 1583. |
| `$F3` | Fade: `$5A=$03CA=$90` ticks to volume 0, then stop. Also clears the hurry tempo boost `$03C8`. |
| `$80-$EF`, `$F4-$FF` | The same fade (`BMI $079F`), but the hurry boost is kept. Verified A: silent after 1 s, out2 = 0. |

Output port 2 carries the **last command received**. It returns to **`$00` when the song ends or is stopped**, so out2 = 0 means "not playing". This is how the player can detect the end of a jingle.

**Port 0, SFX A.** The handler is `$102E`.

- The value is masked `&$7F`. Bit 7 is ignored, so it can serve as a toggle.
- `$7F` (games send `$FF`) is **Hurry up**. It plays the hurry jingle (internally SFX `$1D` on the SFX-C channel). It also sets `$03C8=$0A`, which **raises the tempo of the current song by +$0A**. Verified: SMB1 overworld `$53` went from `$2B` to `$36`.
- The boost stays for songs started later. Songs with IDs `01-06`, `08`, `0D`, `11`, `12`, `14` keep it; every other ID resets it (`$0825-$0840`).
- The "hurry-up variants" of SMB1, LL and SMB3 music are therefore **the same IDs with the tempo boost**, not separate tracks.
- Other values play the SFX from the table at `$17D6`. Output port 0 = the raw value while the SFX plays, then 0.

**Port 1, SFX B.** The handler is `$14C1`.

| Value | Meaning |
|---|---|
| `$FF` | **Enter transfer mode.** Mute and key off (`$0768`), then run the IPL-compatible receive loop at `$0FD2`: out0/1 = `$AA/$BB`, wait for `$CC`, then blocks. It ends by jumping to the exec address, which is always `$0500`. |
| `$F0` | Stop everything and **jump to the IPL ROM `$FFC0`**, a full driver reset. The menu uses this; see section 6.3. |
| other | High nibble goes to `$25`: `$10` starts a looping SFX (LL wind), `$20` stops it. Low nibble: `$01` = **jump**, `$04` = **skid**. Other low values do nothing. |

Output port 1 = `$05 | $26` (current jump/skid ID, OR'd with the loop flag).

**Port 3, SFX C.** The handler is `$11D6`. The value is masked `&$7F`, the SFX pointer table is at `$173A`, and output port 3 = the raw value (bit 7 kept) while it plays, then 0.

### 2.2 CPU-side senders

SMB1/LL (`$04:8163`), SMB2 (`$11:95E4`) and SMB3 (`$22:E677`) use the same code with different RAM. Offsets below are from the queue base `Q`: `$1600` / `$1DE0` / `$1200`.

**Ports 0, 1 and 3:**

* If `Q+n != 0`: write it to the port and keep it in shadow `Q+4+n`. For port 1 the shadow is `&$0F`.
* If `Q+n == 0`: when the APU echo equals the shadow (port 1 compares `&$0F`, port 3 compares `&$7F`), write `$00` and clear the shadow. Otherwise wait, but write `$00` anyway after 3 frames (counters `Q+$0A`, `Q+$0B`, `Q+$0D`). This 0 creates the edge that lets the same SFX be sent again.
* **Port 3 toggle bit.** For IDs **`$01`, `$05`, `$06`, `$4A`**, the value is OR'd with an alternating bit 7 kept at `Q+$0F`. These are coin, 1-up, fireball and one more fast-repeating SFX. Back-to-back identical requests then still differ, and the driver strips bit 7.

**Port 2, music:**

* If `Q+2 != 0`: write it to the port and set the hold counter `Q+$0C = 4`. If the value is below `$F0`, store it as the current music in `Q+6`. Commands `$F0-$FF` do not replace it.
* If `Q+2 == 0`: while the echo on out2 equals `Q+6`, decrement `Q+$0C`. When it reaches 0, write `$00` to port 2, and keep writing `$00` every frame. If the echo differs (song ended or command not yet seen), write nothing.
* All four queue bytes are cleared at the end (`STZ Q..Q+3`). **Only one request per port per frame**: the last write wins.

**Menu, `$00:87CC`.** A simpler version with no retry counter and no hold counter.

- Music: write `$0062` and remember it in `$0066` if below `$F0`. When the echo matches, write 0.
- SFX: write, and when the echo matches the shadow `$0064/$0065/$0067`, write 0.

**Direct writes that skip the queue.** The SMB1 title writes `$14` straight to `$2142` at `$03:9EC7`. The LL title writes `$15` at `$0D:9E4C`.

**Minimum hold time.** When port 2 is driven directly (APU-only tests), a value held for ≥4 ms was taken, but 2 ms was missed at SMB1 tempo. Hold it until out2 echoes it, or for at least one video frame.

---

## 3. Track lists

For "A" results, each ID was played for 20 s. The type is "loop" if it was still playing at 20 s (longer runs are noted), otherwise the time until it stopped. Peak and RMS are the 20 s totals. All IDs were measured APU-only from a ROM-built ARAM (`sweep.py`). Selected IDs were also measured with the game running (§4.3).

### 3.1 All-Stars menu

Bank: the boot stream block to `$C000`, ROM `$3B:9DD3`, `$47E` bytes. **2 songs.**

| ID | Name | Evidence | Type | Peak / RMS |
|---|---|---|---|---|
| `01` | All-Stars title screen theme | S: power-on log, port2=`01` at f542 while the title screen shows; code `$00:9EFE` | loop | 7725 / 1124 |
| `02` | Game Select theme (arrangement of the SMB1 overworld B-section) | S: port2=`02` at f1173 on the SELECT GAME screen; `$00:A3B7` | loop | 10661 / 1210 |
| `80` | fade out (used when a game is chosen) | C: `$00:9F7F` | | |

`03-18` read past the 2-entry table and give garbage: `03`, `06`, `0A`, `0B`, `10` and `18` make noise, the rest are silent. **Do not offer them.**

### 3.2 SMB1 and Lost Levels (shared bank)

Bank: ROM `$1F:8000`, block `$3761` bytes to `$C000`. Table size **21 (`01-15`)**.

The queue is `$7E1602`. The area-music table is `MusicSelectData` at `$05:C8F5` (SMB1) and `$0F:D8F0` (LL): `03 02 06 08 01 13` for water, ground, underground, castle, cloud, pipe-intro.

| ID | Name | Evidence | Type | Peak / RMS |
|---|---|---|---|---|
| `01` | Coin Heaven (cloud bonus area) | C: MusicSelectData[4] (cloud override `$0743`). Also forced when area id `$DB=$1B` (SMB1) or `$39/$3B/$3D` (LL) | loop | 7711 / 1266 |
| `02` | Overworld | S: SMB1 1-1, LL 1-1 | loop | 15167 / 1240 |
| `03` | Underwater | S: 2-2 by death-warp | loop | 7567 / 1272 |
| `04` | Castle clear: Toad's message, worlds 1-7 | C: `$03:D6BA` / `$0D:D5AD` (world ≠ 8) | loop | 14241 / 1759 |
| `05` | Castle clear: Princess rescued, world 8 | C: `$03:D6B6` (world = 8) | loop | 19015 / 2136 |
| `06` | Underground | S: SMB1 1-2, LL 1-2 | loop | 15663 / 2025 |
| `07` | = `06` (same pointer `$CC8B`; not referenced) | A: identical audio | loop | 15654 / 2025 |
| `08` | Castle | S: SMB1 1-4, LL 1-4 | loop | 7731 / 1019 |
| `09` | Death (lose a life) | S: time-up, Goomba | ends 3 s | 7754 / 344 |
| `0A` | Game Over | S | ends 6 s | 6560 / 473 |
| `0B` | World-clear fanfare (axe / bridge collapse) | C: `$03:D4EF`, `$0D:D3D9`; M: C5-G4-E4 arpeggio fanfare | ends 9 s | 7846 / 828 |
| `0C` | Course clear (flagpole) | C: `$03:B952` (with GameEngineSubroutine=5); M: G3 C4 E4 G4 C5 E5 G5 fanfare | ends 7 s | 10816 / 897 |
| `0D` | Starman | C: `$03:DE17`; M | loop | 10689 / 1577 |
| `0E` | Ending (SMB1) | C: `$03:896B` in the ending sequence | loop | 7880 / 1190 |
| `0F` | Ending, non-looping version (about 78 s) | not referenced; M shows the same tune | ends ~78 s | 7880 / 1184 |
| `10` | Ending (Lost Levels) | C: `$0D:870C` | loop | 7880 / 1188 |
| `11` | = `09` (same pointer; not referenced) | A | ends 3 s | 7754 / 344 |
| `12` | = `0A` (same pointer; not referenced) | A | ends 6 s | 6560 / 473 |
| `13` | Pipe intro (the walk-into-pipe lead-in before 1-2 etc.) | S: death-warp to area 1 | ends 3 s | 15169 / 498 |
| `14` | SMB1 title screen | S: `smb1b.state` out2=`14`; direct write `$03:9EC7` | loop | 10322 / 1407 |
| `15` | Lost Levels title screen | S: `ll_title.state`; direct write `$0D:9E4C` | loop | 10218 / 1297 |

- `16` and `17` are silent. `18` is garbage (4 s of noise). These entries point into song `02`'s phrase list.
- **Hurry up** is port0 `$FF` plus the tempo boost, S (`$03:B811` / `$0D:B5C0` at time 100).
- **Pause** is port2 `$F1` plus port0 `$43`; **unpause** is port2 `$F2` plus port0 `$43`. S.
- Death, game over and several transitions use `$80` or `$F3` (fade) and `$F0` (stop).

### 3.3 SMB2

Bank: ROM `$1F:C000`, `$2CCE` bytes. Table size **19 (`01-13`)**. The queue is `$7E1DE2`.

Area music comes from table `$11:B839`, indexed `[level*10 + area]`, with values `01`/`06`/`08`/`07`. It is loaded by `$14:E32E`. Subspace sets area music `05` at `$11:85EE`.

| ID | Name | Evidence | Type | Peak / RMS |
|---|---|---|---|---|
| `01` | Overworld | S: 1-1 | loop | 12551 / 1197 |
| `02`, `03` | = `01` (same pointer `$CC3F`) | A: identical | loop | |
| `04` | Character Select | S: after the title, and after every death | loop | 7113 / 949 |
| `05` | Subspace (SMB1 overworld arrangement) | C: `$11:85EE`; M | loop | 8977 / 1323 |
| `06` | Inside / Underground (caves, buildings, jars) | S: 1-1 area 3 by death-warp; table | loop | 8531 / 1323 |
| `07` | Wart (final boss) | S: 7-2 area 5 (Wart's room) by death-warp; table | loop | 14422 / 1848 |
| `08` | Boss area (Birdo, Mouser, ...) | S: 1-1 area 5 by death-warp; table | loop | 11247 / 1969 |
| `09` | Death (also Bonus Chance "no win", `$11:8FE5`) | S | ends 2 s | 7194 / 414 |
| `0A` | Game Over | S (lives=1 then death) | ends 2 s | 8123 / 389 |
| `0B` | Short fanfare. Probably boss beaten or crystal ball (not scene-verified) | C: object routine `$12:A045` | ends 5 s | 10068 / 688 |
| `0C` | Level clear (end of level, before Bonus Chance) | C: `$11:8E50` | ends 2 s | 9406 / 503 |
| `0D` | Starman | C: `$12:80A0`, `$12:E0EB`; M (same data as SMB1 `0D`) | loop | 10739 / 1577 |
| `0E` | Ending | C: `$12:92B6` (ending sequence timer) | loop | 12840 / 1730 |
| `0F` | = `0C` (same pointer), used on another transition screen | C: `$11:8D64` | ends 2 s | |
| `10` | Item get / 1-UP jingle (mushroom pickup; Bonus Chance win) | C: `$12:B088`, `$11:8FA2` | ends 3 s | 9275 / 682 |
| `11` | Title theme (about 29 s, not looping; the title requests it again) | S: `smb2_title.state` out2=`11`, re-sent at f3128 | ends ~29 s | 10771 / 1078 |
| `12` | unknown (plays music): 3 s fanfare, not referenced by code | A | ends 3 s | 11447 / 625 |
| `13` | Warp (subspace warp screen) | C: `$11:8CBE`, with the world change via `$0635` | ends 4 s | 8831 / 884 |

`14-16` are silent. `17` and `18` are garbage (5 s). SMB2 has no hurry-up.

### 3.4 SMB3: level/title bank

Bank: ROM `$0C:8000`, `$346D` bytes. Table size **24 (`01-18`)**. It is loaded when SMB3 starts, when entering any course, and for the Battle Game. The queue is `$7E1202`.

Level music is set from the level header, byte 10 bits 3-0, through table **`$21:CE46`**:

| Nibble | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | A | B |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ID | `02` | `06` | `03` | `08` | `12` | `10` | `11` | `0C` | `01` | `0F` | `14` | `0F` |

This is done at `$20:9A6A`, which writes the result to `$1061/$1062`.

| ID | Name | Evidence | Type | Peak / RMS |
|---|---|---|---|---|
| `01` | Athletic (1-4, 3-2, 3-3, 3-4 ...) | S | loop | 8878 / 942 |
| `02` | Overworld / plains (1-1) | S | loop | 10305 / 994 |
| `03` | Underwater (3-1, 3-5) | S | loop | 9514 / 1275 |
| `04` | Probably P-switch music. Same data as the toad-house tune (map `0C`) | C: when the tile under the player (`$0603,X`) is `$F2`, `$23:D519` plays SFX P0 `0B`, sets `$0567=$84` and requests `04`. `$23:C066` restores the area music (or star `0D`) when `$0567` counts down. `$0567` is the P-switch counter in the NES SMB3 RAM map; no P-switch was stepped on in-game to confirm. Also used by the W1 toad-house special case at `$20:9A66`. A: identical to map `0C` | loop | 3996 / 641 |
| `05` | Probably Bowser battle | C: `$29:F892` uses the same "start boss fight" pattern as `$28:D918` (which requests `12`); not scene-verified | loop | 14711 / 2157 |
| `06` | Underground (1-5, pipe areas) | S | loop | 15527 / 2024 |
| `07` | Course clear (goal-card fanfare) | C: `$23:F4F3`, `$26:E9B6`, `$26:EA4D`, `$28:CE43`, `$23:9B37`. Identical to map `07` | ends 4 s | 12517 / 614 |
| `08` | Fortress | S: 1-F, 3-F | loop (has rests) | 17804 / 1339 |
| `09` | Death | S | ends 3 s | 10706 / 495 |
| `0A` | Game Over | S (the level bank is still loaded at game over) | ends 3 s | 8169 / 483 |
| `0B` | Victory fanfare. Probably enemy or boss defeated; also Battle Game round won (`$25:F376`) | C: `$27:D364`, `$28:CE5C`, `$28:D711`, `$29:DE96` | ends 6 s | 15070 / 1221 |
| `0C` | unknown short jingle (scale C4-C5) | C: `$28:CE26` | ends 3 s | 11769 / 506 |
| `0D` | Starman | C: `$23:BFF2`, `$28:8D51`; M (same data as SMB1 `0D`) | loop | 10777 / 1577 |
| `0E` | unknown short jingle | The code that writes `0E` (`$20:8341`, `$29:DD85`, `$29:FD69`) is the music-box path, which runs with the map bank loaded. No level-bank use was identified. | ends 3 s | 7972 / 512 |
| `0F` | King's throne room | S: castle tile, "The King has been transformed!" | loop | 6134 / 1037 |
| `10` | Airship | S: airship after leaving the throne room | loop | 9998 / 1636 |
| `11` | Battle: Hammer Bros; also 2P battle when `$1F26≠0` | C: table nibble 6, `$20:A387` | loop | 9600 / 1259 |
| `12` | Koopaling (boss) battle | C: table nibble 4, `$28:D918` | loop | 18272 / 2563 |
| `13` | about 8 s non-looping piece after a boss or in the throne-room sequence. Likely "King restored" or boss defeated | C: `$29:C5CA`, `$29:FDC8` | ends 8 s | 15653 / 1416 |
| `14` | Sky / Coin Heaven. Same tune as map `10` (World 5 sky map) and the body of W9 (`0B`) | C: table nibble A | loop | 7394 / 891 |
| `15` | Title / intro (curtain) | S: `smb3_title.state` out2=`15`; `$20:80A7` | loop | 12634 / 1338 |
| `16` | Battle Game (title menu "BATTLE GAME") | S | loop | 11675 / 1354 |
| `17` | unknown short chromatic jingle (2 s) | C: `$29:C390` (with timer `$0711`, then fade) | ends 2 s | 11004 / 644 |
| `18` | unknown (plays music): 6 s flute arpeggio, not referenced by code | A | ends 6 s | 7191 / 790 |

IDs above `$18` are ignored by the driver.

### 3.5 SMB3: world-map bank

Bank: ROM `$07:C000`, `$1CF6` bytes. Table size **16 (`01-10`)**. It is uploaded whenever a world map is (re)entered. **Toad houses and spade games run on this bank; no upload happens.**

World-map music comes from table `$21:CA0B` = `02 03 04 05 06 08 09 0A 0B` for W1-W9 (`$20:8327`). Special cases: W5 sky section plays `10`; music box active (`$1F55`) plays `0E`.

| ID | Name | Evidence | Type | Peak / RMS |
|---|---|---|---|---|
| `01` | Ending theme (same melody as SMB1 `0E`, longer arrangement) | C: `$20:C8A3`; M | loop | 8547 / 1332 |
| `02` | World 1: Grass Land | S | loop | 11536 / 1174 |
| `03` | World 2: Desert Hill | S | loop | 11299 / 1426 |
| `04` | World 3: Ocean Side | S | loop | 7245 / 974 |
| `05` | World 4: Big Island | S | loop | 7182 / 1254 |
| `06` | World 5: The Sky | S | loop | 7488 / 954 |
| `07` | Course-clear fanfare (same as level `07`) | A | ends 4 s | 12517 / 614 |
| `08` | World 6: Iced Land | S | loop | 7595 / 1295 |
| `09` | World 7: Pipe Land | S | loop | 8569 / 1025 |
| `0A` | World 8: Castle of Koopa | S | loop | 11513 / 1527 |
| `0B` | World 9: Warp Zone (starts with the whistle phrase) | S: map W9, and after using a warp whistle | loop | 5304 / 692 |
| `0C` | Toad house | S | loop | 3996 / 641 |
| `0D` | Spade bonus game ("line up the pictures") | S | loop | 3718 / 856 |
| `0E` | Music Box lullaby | S: item `$0D` used on the map; C: `$20:8341`, `$29:DD85` | loop | 6026 / 444 |
| `0F` | = level `06` Underground (identical data). Map-side use not found | A | loop | 15664 / 2024 |
| `10` | Sky theme: W5 upper (sky) map | C: `$20:8338` (world 5 and sky flag) | loop | 7389 / 891 |

`11` is garbage (3 s). `12-18` are silent.

---

## 4. Sound effects

The SFX data and tables live in the shared driver region, so **SFX IDs are global**: the same number is the same sound in every title.

| Port | Valid IDs | Empty IDs | Notes |
|---|---|---|---|
| 0 (value `&$7F`) | `01-47` (table `$17D6`) | `06`, `11`, `12`, `22`, `24` | `$48-$7E` read garbage. **`$7F`/`$FF` = hurry up.** `21` and `3E` loop. |
| 1 | `01` jump, `04` skid; high-nibble flags `$10` (start loop, the LL wind) and `$20` (stop loop) | other low nibbles | `$FF` = upload mode, `$F0` = driver reset. **Never use either as an SFX.** |
| 3 (value `&$7F`) | `01-4D` (table `$173A`) | `13`, `2B` | `$4E+` reads the port-0 table (garbage). `35` and `4B` loop. |

All IDs were swept APU-only (`sfxsweep.py`). Every valid ID above produced sound except the empty ones listed.

Identified SFX:

| Port/ID | Sound | Evidence |
|---|---|---|
| P0 `01` | block bump (head hit) | S: SMB1 `?` block (`$03:BF31`) |
| P0 `02` | stomp | S: SMB1 Goomba (`$03:DFAE`) |
| P0 `04` | pipe / power-down (injury) | S: SMB1 hit while big (`$03:DF61`); C: pipe entry `$03:AF93` |
| P0 `0A` | power-up collected | C: `$03:DE04` (SMB1 power-up handler) |
| P0 `0E` | swim stroke | C: `$03:B56D` (swimming instead of jump) |
| P0 `19` | message text typing | S: SMB3 toad house |
| P0 `43` | pause / unpause | S: SMB1, sent with port2 `F1`/`F2` |
| P0 `FF` | hurry up (and tempo boost) | S: SMB1 at time 100; SMB3 `$29:E650` |
| P1 `01` | jump | S: SMB1, SMB2, SMB3 |
| P1 `04` | skid | S: SMB1 turn-around at speed (`$04:D81D`); SMB3 uses it too |
| P1 `$10` / `$20` | wind loop start / stop (LL) | C: `$0F:F2F0`; S: `$20` at LL level start |
| P3 `01` | coin (toggle-bit ID) | S: SMB1 `?` block |
| P3 `02` | power-up sprouts from block | C: `$03:BDBD` |
| P3 `03` | vine | C: `$03:BA29` (`$0398` VineFlagOffset) |
| P3 `05` | 1-UP (toggle-bit ID) | S: 100th coin |
| P3 `06` | fireball (toggle-bit ID) | S: fire Mario presses Y |
| P3 `07` | brick shatter | C: `$03:BFAE` |
| P3 `3E` | treasure chest opened | S: SMB3 toad house |

The menu uses P3 `01`, `40`, `41` and P0 `11`, `12`, `3E` for cursor and confirm sounds (power-on log).

Order within a frame matters. If a game writes two SFX to the same port in one frame, only the last is sent: the coin write is overwritten by the 1-UP write.

---

## 5. Recipes: play track N on demand

### 5.1 With the game running (smastool, CPU executing)

1. Load a state of the title whose bank you want.
   - SMB3 level bank: `smb3_title.state`, or any course.
   - SMB3 map bank: a map state such as `re_C/smb3_map1.state`.
2. `poke <queue> F0`, then `run 30`. This stops the current music.
3. `poke <queue> NN`. The NMI sender plays it on the next frame.

Queues: menu `7E0062`, SMB1/LL `7E1602`, SMB2 `7E1DE2`, SMB3 `7E1202`.

Tested (`t_cpu.py`, 10 s each, peak/RMS):

| State | Queue=ID | Peak / RMS | Echo out2 |
|---|---|---|---|
| select | `0062=01` | 7045 / 1040 | 01 |
| select | `0062=02` | 10273 / 1216 | 02 |
| smb1_11 | `1602=02` | 15168 / 1127 | 02 |
| smb1_11 | `1602=06` | 15683 / 1616 | 06 |
| smb1_11 | `1602=0D` | 10689 / 1573 | 0D |
| smb1_11 | `1602=14` | 10325 / 1342 | 14 |
| ll_title | `1602=15` | 6577 / 1244 | 15 |
| ll_title | `1602=03` | 7339 / 1322 | 03 |
| ll_title | `1602=10` | 7656 / 1217 | 10 |
| smb2_11 | `1DE2=01` | 7835 / 1141 | 01 |
| smb2_11 | `1DE2=07` | 11521 / 1906 | 07 |
| smb2_11 | `1DE2=11` | 8738 / 983 | 11 |
| smb2_11 | `1DE2=0E` | 11816 / 1739 | 0E |
| smb3_title | `1202=02` | 9972 / 987 | 02 |
| smb3_title | `1202=10` | 9617 / 1576 | 10 |
| smb3_title | `1202=12` | 17502 / 2511 | 12 |
| smb3_title | `1202=05` | 14733 / 2180 | 05 |
| smb3_map1 | `1202=02` | 11713 / 1170 | 02 |
| smb3_map1 | `1202=0A` | 11484 / 1482 | 0A |
| smb3_map1 | `1202=0C` | 3942 / 657 | 0C |
| smb3_map1 | `1202=01` | 8563 / 1313 | 01 |

Caveat: in a live course the game may override your track. In SMB3 1-1 a Goomba killed Mario, and music `09` followed by the reloaded level music replaced it. Use a quiet scene (title, map, menu) or the APU-only method.

### 5.2 APU only, the recommended way for the Music Player and Sound Select

This is verified with `re_C/apuonly.exe`. It links the project's own `Apu`/`Dsp` sources and never runs the 65816.

**Option A: build ARAM from ROM only. No save state is needed.** See `re_C/buildaram.py`.

1. Start with a 64 KB zeroed image.
2. From the boot stream, copy the blocks at **`$3D00`** (`$9C` bytes), **`$3EE8`** (`$18`) and **`$0500`** (`$210F`, the driver).
   - The boot stream is ROM `$07:FC00` (`$400` bytes) followed by `$3B:8000`. Both are copied back to back into WRAM `$7F:0000` by `$00:8A4F`.
   - Skip its `$3C00` and `$4000` blocks; those are the intro-only sample set.
   - Also copy its `$C000` block if you want the **menu** songs.
3. Copy the main sample set from ROM `$0B:8000`: the `$3C00` block (`$70` bytes) and the `$4000` block (`$7C20` bytes).
4. Copy the title's song block to `$C000`:

   | Title | ROM | Bytes |
   |---|---|---|
   | SMB1/LL | `$1F:8000` | `$3761` |
   | SMB2 | `$1F:C000` | `$2CCE` |
   | SMB3 level | `$0C:8000` | `$346D` |
   | SMB3 map | `$07:C000` | `$1CF6` |

   Each ROM stream is `[len16][addr16][data]...[0000][exec16]`.
5. Set up the APU:
   - `apu.reset(true)`, then `memcpy(apu.ram, image, 0x10000)`.
   - Set `apu.r.pc = 0x0500` and `apu.r.sp = 0xEF`.
   - **Run the APU for at least 20 ms before writing any port.** The driver init writes `$F1=$F0`, which clears the input latches, so earlier writes are lost. Measured: 0 or 5 ms fails, 20 ms works; use 50 ms.
6. Play:
   - `apu.inPorts[2] = N`. Run the APU until `apu.outPorts[2] == N` (a few ms), or at least one frame.
   - Optionally set `inPorts[2] = 0` afterwards. Leaving N there is harmless: 20 s and 90 s holds showed no retrigger.
7. Output: `Apu::runTo()`. The SPC clock is about 1.024 MHz with 32 cycles per sample, giving 32040 Hz stereo in `apu.samples`.

Tested this way from ROM-built ARAM (10 s):

| Bank / ID | Peak / RMS |
|---|---|
| menu `02` | 10510 / 1211 |
| SMB1 `02` | 15170 / 1127 |
| SMB2 `07` | 11322 / 1905 |
| SMB3L `12` | 16286 / 2488 |
| SMB3M `0A` | 11460 / 1472 |

Per-second RMS matches the in-game recordings.

**Option B: start from a save state's APU.** `Snes::loadState` restores the full APU: 64 KB ARAM, SPC registers, timers and DSP.

- The music already playing continues with the CPU stopped. Verified: `smb1_11` overworld ran 10 s APU-only with RMS 1085.
- To switch bank, use one of these:
  - **Host upload**: set port1=`$FF`, wait for out0/out1 = `$AA/$BB`, then do the standard IPL-style block transfer. The kick is `$CC`, then index echo per byte, next kick = last index + 3, and the final block has len 0 and exec `$0500`. Verified: SMB3 map bank uploaded into the `smb3_title` APU, then map `02` played.
  - **Direct copy**: write DSP KOFF=`$FF`, wait about 20 ms, copy the block into `ram`, set PC=`$0500`. Verified: same per-second RMS as the protocol path. Without KOFF there is a tiny click: peak 60 against 33 with KOFF.
- **Pitfall in this codebase:** `Apu` holds `Dsp dsp(ram)`, a pointer into its own RAM. Copy-constructing or assigning an `Apu` makes the copy's DSP read the *original's* RAM. For a separate player instance, create a new `Apu` and move state with `saveState()`/`loadState()`, or copy `ram` plus the DSP state explicitly.
- A state taken during hurry-up carries `$03C8=$0A`, and later tracks play faster. Restarting the driver (PC=`$0500`, which clears RAM) removes it. So do `F3`, or starting any ID outside the keep-list.

### 5.3 Stop, fade, replay

| Action | How | Measured (APU-only) |
|---|---|---|
| Stop now | port2 = `$F0` | silent within about 0.2 s; out2 becomes 0 |
| Fade out | `$F3` or `$80` | about 1 s fade, then out2 = 0 |
| Pause dim | `$F1`, restore with `$F2` | |
| Replay the same ID | write `$00`, wait ≥1 frame (or until the driver reads it), then write the ID again | |
| Switch tracks | write the new ID directly; no 0 needed | |
| End of a non-looping track | out2 drops to `$00` | |
| Silence SFX | no dedicated command; SFX end by themselves. Port1 `$20` stops the looping wind. Port1 `$F0` resets the whole driver: see §6.3, do not use it in the player | |

---

## 6. Driver uploads

### 6.1 When uploads happen

All uploads go through the shared CPU routine **`$00:8BAC`**, the standard SPC IPL protocol. It waits for `$BBAA` on `$2140/41`.

| When | Code | Source | Blocks → ARAM |
|---|---|---|---|
| Power-on (frame 0-24) | `$00:8A4F` DMAs ROM `$07:FC00` (`$400`) and `$3B:8000` (`$8000`) to `$7F:0000`, then uploads through the real IPL | boot stream | `$3D00`, `$3EE8`, `$0500` driver, `$C000` menu songs, `$3C00`+`$4000` intro samples; exec `$0500` |
| Title screen appears (f488), and entering SELECT GAME (f1052) | `$00:8A8E` / `$00:8ABC` (port1=`$FF` then upload) | ROM `$0B:8000` | `$3C00` directory, `$4000-$BC1F` main samples |
| A game is started from its file menu | `$00:89F9` → `JSR ($89F1,X)` with X=`$D0*2` | SMB1/LL `$1F:8000`, SMB2 `$1F:C000`, SMB3 `$0C:8000` | `$C000` song block; exec `$0500` |
| SMB3 map (re)entered | `$00:8B63` (from `$20:80ED` when `$1503≠0`, and `$20:C61D`) DMAs `$2800` bytes of ROM `$07:C000` to `$7F:0000`, then uploads | map bank | `$C000` |
| SMB3 course, Battle Game | `$00:8B47` (from `$20:8677`, sets `$1503`; and `$25:F069`) | level bank `$0C:8000` | `$C000` |
| Return to the menu ("Save & Quit" and similar) | `$00:80E9` / `$00:8144`: clear queues and ports, port1=`$F0` (driver jumps to IPL `$FFC0`), then the full boot upload `$00:83B6`→`$8A4F` | boot stream | as at power-on |

Every block transfer ends with exec `$0500`. The driver re-initialises, clearing `$00-$DF` and `$0200-$03FF`, so music always stops during an upload. SMB1, LL and SMB2 never upload again after game start. **SMB3 switches between its two banks at every map↔course transition**, and each switch takes about 13 frames (map bank) or 23 frames (level bank) of transfer.

### 6.2 Is the saved APU state enough?

Yes. A save state holds the complete ARAM, including the driver, samples and current bank. With it, the current title's music can be played with APU emulation only (§5.2 B).

For SMB3, the state only holds the bank of the scene where it was saved:

- `smb3_title`, and states taken in a course or the Battle Game, hold the **level bank**.
- Map, toad-house and spade states hold the **map bank**.

To offer every SMB3 track, swap the `$C000` block (§5.2), or build ARAM from ROM (§5.2 A). Option A needs no state at all.

### 6.3 Emulator issue found while testing (outside this task's scope)

The APU boot ROM is implemented as a state machine in `src/core/apu.cpp` (`iplStep`). When the driver itself jumps to `$FFC0` after port1=`$F0`, that state machine does **not** restart at "announce `$AA/$BB`". `iplState_` stays at 2/3.

The menu-return path `$00:80E9` then waits forever at `$00:8BB5`, comparing `$2140` against `$BBAA`. **Reproduced:**

1. SMB1 with lives=0, run the timer out.
2. Choose "SAVE&QUIT" on the Game Over screen.
3. Result: black screen. The CPU loops at `$00:8BB8`; the SPC is at PC `$FFC0` with in-ports `00 F0 00 00`.

The IPL entry state needs to reset when execution enters `$FFC0` with `$F1` bit 7 set. I did not change any code.

Separately, while teleporting Mario on SMB3 maps with `$0047`/`$0043`, walking into map enemies (W2 Hammer Bro, W8 tank) led to crashes after the bank upload (PC `$5C:3505` / `$00:0001`). This is probably caused by my teleport pokes, but it has not been ruled out.

---

## 7. How the findings were produced (reproducible; scripts in `re_C/`)

| File | Purpose |
|---|---|
| `spcdis.py` | SPC700 disassembler for `.aram` dumps |
| `nspc.py`, `nspc2.py` | N-SPC song decoder (phrases, tracks, notes); used for the M evidence |
| `apuonly.cpp` / `build_apuonly.bat` → `apuonly.exe` | APU-only harness built from `src/core/*.cpp` (sources are not modified). Commands: `port`, `run MS`, `upload ROMADDR` (host transfer protocol), `direct`, `reset`, `loadaram`, `setpc`, `dspw`, `wav`, `stats`, `out`, `peek`/`poke` |
| `buildaram.py` | builds `aram_{menu,smb1,smb2,smb3L,smb3M}.bin` purely from ROM |
| `sweep.py <bank> <maxid> <secs>` | plays every ID APU-only and reports peak, RMS and per-second RMS. Results in `sweep_menu.txt`, `sweep_all.txt` |
| `sfxsweep.py <port> <maxid>` | SFX sweep. Results in `sfx_p0.txt`, `sfx_p1.txt`, `sfx_p3.txt` |
| `t_cpu.py` | queue-poke recipe tests with the CPU running |
| `play.py` | scripted input plus `watch` on the queues |
| `ctx.py` | disassembly context before a ROM address |

Extra states made for testing:

- `re_C/smb1_*`-style tests use the provided states.
- New states: `smb2_11.state` (SMB2 1-1), `smb3_map1.state`, `smb3_map2.state`, `smb3_map3.state`, `smb3_map8.state` (world maps), `smb3_11.state`, `smb3_toad.state`, `smb3_battle.state`, `smb3_afterking.state`, `smb3_airship.state`, `ll_11.state`.

Scene tricks:

| Title | Trick |
|---|---|
| SMB1/LL | **Death-warp**: set timer `$07E9-$07EB` to 002, wait 120 frames, poke `$075F` world, `$075C` level and `$0760` area. The next reload loads that area. |
| SMB2 | **Death-warp**: set `$001E=2` (drop into a pit), then poke `$0533`/`$04E8`/`$0531` level, `$0534`/`$04E9`/`$0532` area and `$0635` world. Press A at character select. |
| SMB3 | **Map teleport**: `$0047` X, `$0043` Y, then press A. **World select**: die (timer `$05EE-$05F0`=001) and poke `$0727` before the map reloads. |


## Which voices are the music's ($1A)

The driver keeps a bit mask of the voices that sound effects hold at ARAM **`$1A`**. Each effect port's handler sets its voice's bit when the effect starts and clears it when it ends (port 1 jump/skid: voice 7, `$150C` SET1 / `$154A` CLR1; port 0: voice 4, `$1085` / `$10D2`; port 3: voices 5 and 6, `$1407`/`$13C1` and `$1208`/`$1231`; others at `$1747`, `$1854`, `$15E4`). The music code reads it (`$0614`, `$06F7`, `$0990`, `$0A3A`, `$0E59`, ...) and leaves those voices alone, so at any moment the voices not in `$1A` are the music's. Found by diffing ARAM around a jump and tracing the SPC700 (smastool `aram`, `spctrace`).

The port's Music Volume / Sound Effects Volume use it (`Apu` copies `$1A` into `Dsp::sfxVoices` before each sample, `Dsp` scales each voice's contribution to the main and echo mix): music off leaves exact silence between effects, and music-off plus effects-off adds up to the normal mix (verified on SMB1, LL, SMB2 and SMB3 clips; the games' RAM is unchanged).
