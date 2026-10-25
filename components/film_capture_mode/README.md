# film_capture_mode

Chooses the camera formats for the connected sensor. The component has no
hardware dependencies and no internal state.

## Responsibilities

- Choose the viewfinder format and the number of stream buffers.
- Decide whether stills use a JPEG sensor mode or the viewfinder frame.
- Report the resulting photo size.

## Inputs and outputs

`film_capture_mode_plan()` takes:

- the sensor's formats, which `film_sensor` enumerates on the device;
- whether the sensor has a JPEG still path with exposure and white-balance
  control (currently OV3640 only);
- free PSRAM and the largest free PSRAM block, measured before the stream opens.

It returns indexes into the format array, so the caller can apply the chosen
format with `VIDIOC_S_SENSOR_FMT`. An index of `-1` means "keep the sensor's
current format" for the viewfinder, or "use the viewfinder frame" for stills.

## Policy

- **Viewfinder candidates.** A format qualifies when it is UYVY, landscape,
  runs at 10 fps or more, and its centred 4:3 window is at least 480×360. That
  window is computed the same way as `film_viewfinder_window_43`.
- **Sensors with a JPEG still path.** Prefer the highest frame rate, then the
  smallest frame. Stills use the largest 4:3 JPEG format.
- **Sensors without a JPEG still path.** The viewfinder frame becomes the
  photo. Prefer the most 4:3-window pixels, then a frame rate closest to 15 fps,
  then the smallest frame.
- **Memory fit.** Take the first candidate that fits PSRAM, trying two stream
  buffers before one. The fit counts:
  - one extra frame for the DVP driver's backup buffer;
  - the 4:3 window copied at the shutter;
  - a 1 MB reserve for the UI.

## Usage

```c
film_capture_mode_t modes[FILM_CAPTURE_MODE_MAX];
size_t count = 0;
/* fill modes[] from VIDIOC_ENUM_SENSOR_FMT */
const film_capture_env_t env = {
    .jpeg_still = true,
    .psram_free = free_bytes,
    .psram_largest = largest_block,
};
film_capture_plan_t plan;
film_capture_mode_plan(modes, count, &env, &plan);
```

Host tests: `host_test/test_film_capture_mode.c`.
