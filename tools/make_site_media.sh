#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RELEASE_DIR="${PROJECT_DIR}/release"
CLIPS_DIR="${PROJECT_DIR}/promo/public/clips"
PRINTS_DIR="${PROJECT_DIR}/promo/public/prints"
DOCS_DIR="${PROJECT_DIR}/docs/assets"

DEMO="${RELEASE_DIR}/mosaico_film_demo.mp4"
POSTER="${RELEASE_DIR}/mosaico_film_demo_poster.jpg"
CONTACT="${RELEASE_DIR}/mosaico_film_demo_contact.jpg"

for tool in ffmpeg; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "Missing required tool: ${tool}" >&2
        exit 1
    fi
done

for source in "${DEMO}" "${POSTER}" "${CONTACT}"; do
    if [[ ! -f "${source}" ]]; then
        echo "Missing release source: ${source}" >&2
        exit 1
    fi
done

mkdir -p \
    "${DOCS_DIR}/fonts" \
    "${DOCS_DIR}/gif" \
    "${DOCS_DIR}/img" \
    "${DOCS_DIR}/loops" \
    "${DOCS_DIR}/prints" \
    "${DOCS_DIR}/video"

echo "Encoding the web demo..."
ffmpeg -y -loglevel error -i "${DEMO}" \
    -vf "scale=720:-2:flags=lanczos" \
    -c:v libx264 -preset slow -crf 25 -pix_fmt yuv420p \
    -c:a aac -b:a 128k -movflags +faststart \
    "${DOCS_DIR}/video/mosaico_film_demo.mp4"
cp "${POSTER}" "${DOCS_DIR}/video/mosaico_film_demo_poster.jpg"

echo "Building cover and social images..."
ffmpeg -y -loglevel error -i "${POSTER}" -frames:v 1 \
    -filter_complex \
    "[0:v]scale=1600:900:force_original_aspect_ratio=increase,crop=1600:900,gblur=sigma=36[bg];\
[0:v]scale=-2:900[fg];[bg][fg]overlay=(W-w)/2:0" \
    -q:v 3 "${DOCS_DIR}/img/cover.jpg"
ffmpeg -y -loglevel error -i "${POSTER}" -frames:v 1 \
    -filter_complex \
    "[0:v]scale=1200:630:force_original_aspect_ratio=increase,crop=1200:630,gblur=sigma=32[bg];\
[0:v]scale=-2:630[fg];[bg][fg]overlay=(W-w)/2:0" \
    -q:v 3 "${DOCS_DIR}/img/og-cover.jpg"
ffmpeg -y -loglevel error -i "${CONTACT}" -frames:v 1 \
    -vf "scale='min(1600,iw)':-2:flags=lanczos" \
    -q:v 3 "${DOCS_DIR}/img/contact.jpg"

encode_loop() {
    local name="$1"
    local source="${CLIPS_DIR}/${name}"
    if [[ ! -f "${source}/0000.jpg" ]]; then
        echo "Missing clip frames: ${source}" >&2
        exit 1
    fi
    ffmpeg -y -loglevel error -framerate 30 -i "${source}/%04d.jpg" \
        -vf "scale=480:480:flags=lanczos" \
        -an -c:v libx264 -preset slow -crf 23 -pix_fmt yuv420p \
        -movflags +faststart "${DOCS_DIR}/loops/${name}.mp4"
    ffmpeg -y -loglevel error -i "${DOCS_DIR}/loops/${name}.mp4" \
        -frames:v 1 -q:v 3 "${DOCS_DIR}/loops/${name}.jpg"
}

echo "Encoding interaction loops..."
for clip in m6_shot picker sx_develop redevelop share; do
    encode_loop "${clip}"
done

echo "Building the README interaction GIF..."
ffmpeg -y -loglevel error \
    -framerate 30 -start_number 0 -i "${CLIPS_DIR}/m6_shot/%04d.jpg" \
    -framerate 30 -start_number 76 -i "${CLIPS_DIR}/redevelop/%04d.jpg" \
    -framerate 30 -start_number 0 -i "${CLIPS_DIR}/share/%04d.jpg" \
    -filter_complex \
    "[0:v]trim=duration=2.2,setpts=PTS-STARTPTS[a];\
[1:v]trim=duration=2.2,setpts=PTS-STARTPTS[b];\
[2:v]trim=duration=2.2,setpts=PTS-STARTPTS[c];\
[a][b][c]concat=n=3:v=1:a=0,fps=10,scale=280:280:flags=lanczos,split[x][y];\
[x]palettegen=max_colors=96[p];[y][p]paletteuse=dither=bayer" \
    "${DOCS_DIR}/gif/film_flow.gif"

declare -a print_names=(
    films_0 films_1 films_2 films_3
    films_4 films_5 films_6 films_7
)

echo "Optimizing film samples..."
for name in "${print_names[@]}"; do
    source="${PRINTS_DIR}/${name}.jpg"
    if [[ ! -f "${source}" ]]; then
        echo "Missing print: ${source}" >&2
        exit 1
    fi
    ffmpeg -y -loglevel error -i "${source}" -frames:v 1 \
        -vf "scale='min(800,iw)':-2:flags=lanczos" \
        -c:v libwebp -quality 82 "${DOCS_DIR}/prints/${name}.webp"
done

cp "${PROJECT_DIR}/tools/assets/fonts/Jost.ttf" "${DOCS_DIR}/fonts/Jost.ttf"
cp "${PROJECT_DIR}/tools/assets/fonts/DSEG7Classic-Regular.ttf" \
    "${DOCS_DIR}/fonts/DSEG7Classic-Regular.ttf"

echo "Site media written to ${DOCS_DIR}"
