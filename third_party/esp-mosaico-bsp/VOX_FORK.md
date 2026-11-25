# VOX M-01 fork of `esp-mosaico-bsp`

Copy of `submodule/esp-mosaico-bsp/components/esp-mosaico-bsp` at BSP commit **`5f88b91`**
(Apache-2.0, Espressif; see `LICENSE`). It is used through `override_path` in
`main/idf_component.yml`, so the shared submodule is never modified.

## The only change

`onboard/display.c`, after `io_config.flags.psram_dma_direct = false;`:

```c
io_config.trans_queue_depth = 2;
```

- **Why.** With `psram_dma_direct = false`, every queued color transfer to the CO5300 holds its own
  internal DMA bounce buffer: 8 lines, 7 680 B. The CO5300 driver defaults to
  `trans_queue_depth = 10`, so up to 10 of those buffers can be held at once. That fragments the
  internal DMA heap, and the panel freezes when the largest free block falls below one transfer
  (VOX HANDOFF §5, minimum observed 8 192 B).
- **Source.** The same one-line change is applied by openai-live-esp32 to this BSP commit for
  ESP-Mosaico (`examples/mosaico`, `tools/prepare_mosaico.py`, Apache-2.0). Their notes say it keeps
  TE-synced, tear-free full-screen updates.

## Maintenance

- `python tools/check_bsp_fork.py` (from the project root) fails if this copy differs from the
  submodule in anything other than that change.
- When the submodule BSP is updated:
  1. copy the component again;
  2. re-apply the line;
  3. update the commit above;
  4. run the check.
- Drop the fork once the BSP exposes the queue depth (proposal to upstream pending).

## Camera components (1.4.0+)

- `third_party/mosaico_module_camera` and `third_party/mosaico_module_mgr` are verbatim copies, so
  that their `../esp-mosaico-bsp` dependency resolves to this fork. Unit-test folders are not
  copied.
- **Official fix taken from upstream (test 13):** `mosaico_module_camera/mosaico_camera.c` and its
  `README.md` come from esp-mosaico-bsp **`b60f5d8`** (2026-10-19, "fix: update camera app"),
  instead of the submodule commit `5f88b91`.
  - With that fix the module drives CAM_PWDN (GPIO48) itself, with board polarity: 0 normal,
    1 sleep, SC101IOT included.
  - Before it, the generic sensor driver drove that pin with its own polarity.
  - With `5f88b91`, VOX's SC101IOT answered over SCCB but never delivered a frame
    (`VIDIOC_DQBUF` timeout), and its red LED followed the wrong PWDN level.
  - The only other change in `5f88b91..b60f5d8` is a display drive-strength default in the BSP
    Kconfig. It is unrelated and deliberately not taken.
- `tools/check_bsp_fork.py` compares those two files with `git show b60f5d8:…` in the submodule
  repository, and the rest with the submodule checkout.

