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
- `components/film_filter` — eight deterministic film looks.
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
