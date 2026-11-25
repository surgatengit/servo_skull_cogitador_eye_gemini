# app_photos

Persistent ring of the latest camera stills for the eye's gallery (1.5.0). Kconfig:
`VOX_PHOTOS_SLOTS` (10).

## Storage

- Partition **`photos`** (data, subtype 0x41), 512 KiB at 0xD80000. It was carved out of `main_app`,
  which went from 12 224 K to 11 712 K in 1.5.0. System Update writes the new table.
- Up to 10 slots of 48 KiB. Each slot is a 32-byte header (`VOXP`, version, sequence, size, CRC32)
  followed by the JPEG.
- Save order: erase the slot, write the JPEG, then **write the header last**. A cut save leaves no
  valid header, so the slot is simply empty.
- At boot only the headers are scanned. A read checks the CRC, so damaged slots are skipped with
  `ESP_ERR_INVALID_CRC`.
- The ring is pure logic (`photos_ring.c`), tested on the PC (`tools/host_tests/run.ps1`):
  - filling;
  - replacing the oldest;
  - reusing holes;
  - order by sequence number.
- Leftover bytes from the old app area are ignored: no valid magic or version means an empty slot.

## Contract

- **Single owner:** the VOX session task. Flash erase and write must not run on a task whose stack
  is in PSRAM (see HANDOFF §5). The GSP task never calls this component: the gallery reads go
  through the session.
- `app_photos_init()` returns `ESP_ERR_NOT_FOUND` when the partition is missing (old table). After
  that, count is 0 and saves are refused, so VOX keeps working without a gallery.
- `app_photos_save()` erases one 48 KiB slot (about 0.5 s). The session calls it after the still was
  sent to Gemini.
- `app_photos_read(position, …)`: position 0 is the oldest and count-1 the newest.
- Privacy: the stills stay on the device until they are overwritten. They are never sent anywhere
  else (Gemini gets the still once, when it asks for it).
