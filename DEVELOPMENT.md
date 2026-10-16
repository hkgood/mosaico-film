# Mosaico Film development

[中文 README](README.md) · [English README](README.en.md)

Mosaico Film is developed as an application inside the
[`esp-mosaico-vibe`](https://github.com/esp-mosaico/esp-mosaico-vibe)
workspace. The application owns its film UI and processing pipeline; board,
camera, GSP and retained Vibe Mode integration come from the workspace BSP and
utils submodules.

## Requirements

- Python 3.10 or newer
- ESP-IDF at the revision pinned by the workspace, targeting `esp32s31`
- Initialized `submodule/esp-mosaico-utils` and
  `submodule/esp-mosaico-bsp`
- A C11 compiler and CMake for host tests
- Node.js 18+ and FFmpeg only when rebuilding promotional/site media
- ESP-Mosaico with an OV3640 camera module in the left slot for hardware use

Clone the application at the path expected by its workspace-relative component
overrides:

```sh
git clone https://github.com/esp-mosaico/esp-mosaico-vibe.git
cd esp-mosaico-vibe
git submodule update --init \
  submodule/esp-mosaico-utils \
  submodule/esp-mosaico-bsp
git clone https://github.com/hkgood/mosaico-film.git projects/mosaico_film
```

All commands below that invoke `mosaico.py` run from the workspace root.

## PC preview

The GSP preview displays the same 480×480 canvas and uses the portable
`film_ui` code with the PC implementation of `film_port_t`:

```sh
python mosaico.py project sim \
  --project projects/mosaico_film \
  --interactive
```

`pc/assets/sensor_mock.ppm` provides the deterministic mock camera frame. The
preview does not emulate camera timing, NAND throughput, Wi-Fi or hardware
feedback exactly; those remain hardware validation items.

## Host tests

The host suite builds the portable filter, darkroom, library and full UI
interaction harness:

```sh
cmake -S projects/mosaico_film/host_test \
      -B projects/mosaico_film/build_host
cmake --build projects/mosaico_film/build_host --parallel
ctest --test-dir projects/mosaico_film/build_host --output-on-failure
```

For memory and undefined-behavior checks:

```sh
cmake -S projects/mosaico_film/host_test \
      -B projects/mosaico_film/build_host_sanitize \
      -DFILM_SANITIZE=ON
cmake --build projects/mosaico_film/build_host_sanitize --parallel
ctest --test-dir projects/mosaico_film/build_host_sanitize --output-on-failure
```

## Firmware build and installation

Build with the workspace's pinned ESP-IDF environment:

```sh
idf.py -C projects/mosaico_film build
```

Use a complete system update for a new installation, partition changes or
changes to `components/film_assets/assets.bin`. This command also builds and
validates the system-update manifest before connecting to a device:

```sh
python mosaico.py iris system-update --project projects/mosaico_film
```

Use `iris app-update` only for code-only changes when the complete partition
table and external assets are unchanged. Run `python mosaico.py recover` first
for a blank or unverified device, or when neither normal firmware nor Vibe Mode
is reachable after connection checks.

Do not use `idf.py flash`, `esptool` writes or guessed flash offsets. The
system-update bundle preserves the immutable retained-firmware prefix and
writes the images declared by the application manifest.

## Architecture

```text
ui/main.json
    └── GSP Canvas (480×480 RGB565)
          ↑ complete frame
components/film_ui
    ├── camera, picker, album, detail, redevelop and share screens
    ├── film_library metadata
    └── film_port_t platform boundary
          ├── pc/film_port_pc.c
          └── main/film_port_dev.c
                ├── camera / still capture
                ├── lab / JPEG writer
                ├── NAND photo storage
                ├── Wi-Fi / HTTP sharing
                └── sound, haptics, battery and IMU

components/film_darkroom
    └── crop, orientation, scaling, development and print composition
components/film_filter
    └── curves, color, halation, vignette, leaks, grain and pixel film
components/film_assets
    └── memory-mapped fonts, textures and boot animation
```

### Ownership and threading

- `film_app_t` has one UI-thread owner.
- `film_port_t` calls made by the UI return immediately; platform tasks report
  completion through `poll_event()`.
- Preview frames remain owned by the platform until `preview_release()`.
- Developed screen buffers remain owned by the platform until
  `release_result()`.
- `main/film_lab.c` owns the capture/develop queue; `main/film_writer.c` owns
  JPEG encoding and persistent writes.
- A `film_filter_handle_t` is single-owner. Its optional worker joins before
  `film_filter_develop()` returns.

### Deferred development and power-loss resume

Shooting and full-resolution development are decoupled because the darkroom
needs most of the PSRAM and both cores, so it cannot run beside the camera.

1. Shutter: the camera captures the 2048×1536 JPEG and the lab writes it as
   `MFnnnnnn_RAW.JPG`, then writes the 36-byte marker `MFnnnnnn.PND`
   (`film_pending.h`: shot parameters + CRC32). The UI is told `SHOT_DONE` and
   the viewfinder restarts immediately.
2. A small proof (screen-size preview) is developed right away so the shot
   animation has something to show.
3. The full-resolution develop is queued (`PENDING_MAX`). It starts only after
   `QUIET_MS` without shutter/UI activity. A new shutter aborts a running
   develop cooperatively (`abort_request`, checked in the JPEG source read);
   the job returns to the queue and restarts from the RAW file later.
4. When the photo is finished the writer removes the `.PND` marker.
5. On boot `film_pending_recover()` collects valid markers, deletes broken
   markers / incomplete RAW files and resumes the rest. Finished photos are
   never touched. The library never reuses a pending id
   (`film_library_max_id_on_disk()`).

`film_port_t.pending_count` lets the UI show `DEV n` under the counter.
`film_lab_reserve()` rejects only when the pending queue is full.
Development is also postponed while the battery is ≤5 % and not charging
(`can_develop`); the markers keep the work for the next boot or charge.
Post-shot animations (M6 hold/reveal/fly, SX-70 developing page) do not wait
for the full-resolution develop and are skipped by the next shutter or tap.

### Camera boards and "camera not connected"

Two camera boards exist in the field and `film_camera.c` has to cope with both:

| Board | Viewfinder | Still |
| --- | --- | --- |
| OV3640 | UYVY 640×480 | 2048×1536 JPEG (`film_still.c`) |
| SC101IOT | UYVY 640×480 (15 fps profile) or the 1280×720 default | the viewfinder frame, JPEG-encoded by the lab (`film_still_t.from_frame`), so 640×480 / 960×720 |

Root cause of the reported "camera not connected": the firmware built only the
SC101IOT 1280×720 profiles while the app requested 640×480, so
`mosaico_camera_open` returned `ESP_ERR_NOT_SUPPORTED` (format mismatch) and the
retry loop never succeeded. `sdkconfig.defaults` now also enables
`CONFIG_CAMERA_SC101IOT_DVP_YUV422_640X480_15FPS`; existing build directories
need that symbol set (or `idf.py fullclean`).

`camera_attach` is a small ladder: 640×480 first, then the sensor default
(`0×0`) when the format is not supported; a present module whose EEPROM
descriptor is invalid is claimed with `allow_unidentified` (the sensor ID check
still rejects anything that is not an OV3640/SC101IOT). 16:9 frames are reduced
to their centred 4:3 window (`film_viewfinder_window_43`) before cropping, so
viewfinder, proof and still share one geometry. The reason for a failed attach
is logged once per change and shown in Settings → About (`NO MODULE`,
`BAD EEPROM`, `NOT A CAMERA`, `ERR …`), which is the first thing to ask users
for. SC101IOT behaviour is not verified on hardware in this repository.

### Settings

`film_settings.h` is the persisted struct (NVS `film/settings`). It only ever
grows at the end; `film_settings_read` tries the current size and then the
historical sizes (8 and 10 bytes) so upgrades keep roll and frame counters.
Reachable from the camera picker (pull down from the top edge → SETTINGS):
skip boot animation, sound, vibration, film viewfinder, screen-off time
(30 s / 1 / 2 / 5 min; dimming at half) and an About card. `main.c` reads the
settings before the boot animation to apply mute/vibration and to skip the
animation; `film_feedback_set_mode` applies them at runtime.

Touch targets: each settings row is a 52 px full-width card and toggles on
release (a finger that drifts or rests a little still counts as long as it
stays on the same row). The picker's SETTINGS row is 52 px tall with a 12 px
hit slop.

### Never walk the whole filesystem

`lfs_fs_size` (behind `esp_littlefs_blockdev_info`) reads every data block of
every file, one NAND read each, while holding the LittleFS lock. It scales with
the number of photos (~80 s at 136 photos) and used to run at mount (boot
stalled ~30 s), in the render path (UI froze 30–47 s) and in the writer task
(after a shot: task watchdog reset). Nothing may call it. `film_storage_free_bytes()`
returns 0 = unknown (no "N LEFT" hint, no shutter gate; a full volume is reported
by `ENOSPC` → `FILM_ERR_STORAGE_FULL`) and About shows `film_storage_total_bytes()`
(block-device geometry, no filesystem access).

The one unavoidable scan is LittleFS's first block allocation after mount;
`film_storage_warm_up` pays it in the lab task 4 s after boot so the first shot
does not. It still grows with the amount of stored data and blocks every
filesystem call while it runs; a real fix needs a larger LittleFS block size
(reformat), which we never do on a device holding the system-update bundle.

### Viewfinder framing and whole-UI rotation

The sensor is mounted rotated 90° relative to the screen, so the full sensor
frame is a 3:4 portrait picture on screen. Framing follows how the camera is held
(no setting; `app_crop_43()`):

- held upright (0°/180°): 4:3 landscape crop, 1536×1152 photo (the 1.0 way);
- held sideways (90°/270°): the whole sensor frame, which is 4:3 once the UI is
  turned upright, full-pixel 2048×1536 photo.

On the camera screen the whole UI turns with the hold direction, like a phone.
`film_app_render` draws the page in *user coordinates* (upright layout, `app->rot`
temporarily 0) into `ui_buf`, then `gfx_rotate_copy` rotates the damaged part onto
the screen. Pointer coordinates are rotated back (`film_app_pointer`) and
`film_app_damage` rotates the damage rect. The camera task converts viewfinder
frames straight into the user orientation (`film_viewfinder_convert_rot_look`,
`film_preview_config_t.rot`), so frames are always 480×360 in the window. Pages
therefore never rotate individual elements; only code outside `render` (pointer,
damage, preview) knows about the hold direction. `crop_43` in the saved settings
is no longer used for framing.

Which pages rotate is decided in one place, `ui_follows_rot()` in `app_private.h`.
Every page does now (camera with its full-screen camera panel, album, detail,
re-develop, share, film picker, settings). Page code keeps using upright user
coordinates; to exclude a page, special-case it in that function.

### Still capture: do not parse JPEGs with the software decoder

`film_still.c` validates each DVP frame with `header_valid()` (a bounds-checked walk of
the JPEG marker segments up to SOS, checking SOF size) plus SOI/EOI. It used to call
`jpeg_dec_parse_header` on the camera task; the device log of the "reboot right after
the shutter" bug stopped right at that point (reset reason 4, no core dump). The
camera task stack is 10 KB and logs `stack free min` after every capture.

### Settings page speed and scrolling

- Touch no longer forces a full-screen redraw: `film_shell` trusts
  `film_app_step`/`film_app_damage`. Pages that mark their own damage
  (`screen_ops_t.own_damage` + `app_invalidate`, used by the settings page) redraw and
  flush only the row that changed; finger moves that change nothing redraw nothing.
- Interactive saves (settings toggles, film dial) use `app_save_settings_later`: one
  NVS write 0.8 s after the last change instead of one per tap. Shot counters still
  use the immediate `app_save_settings`.
- The settings page scrolls when its content (rows + about card, same width and font)
  is taller than the screen. Drag/inertia invalidate only the area below the header;
  a drag cancels the pressed row, so scrolling never toggles a setting.

### Photos still developing in the album

A photo only enters the library on `FILM_EVT_SAVED` (all files written). Until then
`app_dev.c` tracks it in memory (`dev_item_t`, at most `DEV_ITEMS_MAX`): `SHUTTER_DONE`
creates a queued item, the proof (`DEVELOPED`) is box-shrunk into a thumbnail, `PROGRESS`
fills the bar, `SAVED`/`SHOT_FAILED` remove it (in the same frame the library gains the
photo, so the grid never flickers). The album draws these cells first (dimmed proof,
`QUEUED` or `DEVELOPING` + progress bar); they cannot be opened or selected, a tap only
shows a toast. Photos resumed after a reboot have no proof/progress events and appear
only once saved. Covered by `film_dev_items_test`.

### Film looks and the grid pass

`film_preset_t` has two optional primitives besides the curve/tint/halation/vignette/grain
set: `grain_size` (grain noise coordinates advance slower, so one noise value covers a
bigger block; still normalised to a 1440 px long edge) and `clarity` (large-radius local
contrast). Clarity reuses the halation grid: the first pass stores the mean luminance
instead of the highlight mask, the blur radius is `width / 14` instead of `width / 60`,
and the finish pass adds `clarity * (pixel - local mean)`. A film therefore has halation
**or** clarity (`film_preset_uses_grid()`), both go through the same two-pass/strip/reuse
paths, so strip output stays bit-identical to whole-image output. STRAY 3200 uses both
primitives; its canister art currently reuses SILVER's.
SEPIA 100 needs no primitive: saturation 0 plus shadow/highlight tints (the viewfinder
look LUT folds the tints in, so it matches closely); its canister reuses FADED's.

### Versioning and perf trace

The firmware version comes from `CONFIG_APP_PROJECT_VER` (`sdkconfig.defaults`)
and `project(... VERSION ...)` in `CMakeLists.txt`; keep them equal.
`CONFIG_FILM_PERF_TRACE` (default off) adds periodic timing logs and task
snapshots; leave it off in release builds because it costs frame time itself.

## Module map

### Device application

- `main/main.c` — system startup and retained Vibe Mode OTA support.
- `main/film_shell.c` — GSP canvas lifecycle and UI frame presentation.
- `main/film_camera.c` / `film_still.c` — preview and full-resolution capture.
- `main/film_lab.c` / `film_writer.c` — asynchronous development and storage.
- `main/film_storage.c` — NAND photo library and persisted settings.
- `main/film_share.c` — LAN/hotspot gallery and session-scoped downloads.
- `main/film_feedback.c` — synthesized sound and motor feedback.
- `main/film_power.c` — battery state.
- `main/film_shutter_key.c` — red AI-key shutter input.

### Portable components

- `components/film_ui` — screens, interaction state and `film_port_t`.
- `components/film_filter` — ten deterministic film looks.
- `components/film_darkroom` — viewfinder and full-resolution processing.
- `components/film_gfx` — small RGB565 drawing primitives.
- `components/film_assets` — generated asset table and partition image.
- `components/qrcodegen` — Nayuki QR encoder used by the sharing screen.

## Asset partition

`tools/assets/make_assets.py` builds:

- `components/film_assets/assets.bin`
- `components/film_assets/assets_table.c`
- `components/film_assets/boot_anim.bin`

Run it from the project directory and pass an explicit boot video:

```sh
python tools/assets/make_assets.py --boot-video /path/to/boot.mp4
```

The checked-in binary image allows a clean workspace clone to build without
regenerating all design assets. When it changes, commit the generated table and
binary together and install with `system-update`.

The partition layout is:

```text
ota_0    0x210000  0x7f0000
assets   0xa00000  0x500000
ui_apps  0xf00000  0x100000
```

The prefix through application NVS follows the retained-firmware contract and
must not be moved to make an app-only update pass.

## Promotional film and website media

The Remotion project is under `promo/`. Its timing lives in
`promo/src/timeline.json`, and `promo/src/lib/theme.ts` is the visual source of
truth for the darkroom palette.

Typical sequence:

```sh
cmake -S host_test -B build_host
cmake --build build_host --parallel

cd promo
npm install
npm run render3d
npm run audio
npm run render
cd ..

./tools/make_site_media.sh
```

The final Remotion film is staged in `release/`, which is intentionally ignored.
`tools/make_site_media.sh` produces the smaller, web-ready files committed under
`docs/assets/`. Unsplash source IDs and author credits live in
`promo/assets/unsplash/credits.json`.

## Repository policy

Commit source, tests, generated firmware assets required to build, and selected
`docs/assets` media. Do not commit local builds, ESP-IDF managed components,
promo frame sequences, Node dependencies, local release staging or device
credentials.

Project code is Apache-2.0. Bundled libraries, fonts and photographs retain
their own terms; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
