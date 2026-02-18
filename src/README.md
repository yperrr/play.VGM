# Playdate VGM Player — Proof of Concept

A video game music player for the [Playdate](https://play.date/) handheld console, powered by [vgmstream](https://github.com/vgmstream/vgmstream).

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                  Playdate Hardware                    │
│  ARM Cortex-M7 @ 180MHz  │  16MB RAM  │  400×240 1-bit │
├─────────────────────────────────────────────────────┤
│                                                       │
│  ┌──────────┐    PCM int16    ┌──────────────────┐  │
│  │ vgmstream ├───────────────►│ Playdate Audio   │  │
│  │ (decode)  │                │ Engine           │  │
│  └─────┬────┘                │ (addSource CB)   │  │
│        │                      └──────────────────┘  │
│        │ reads via                                    │
│  ┌─────▼────────────┐                                │
│  │ pd_streamfile     │  ◄── bridges vgmstream I/O    │
│  │ (FS adapter)      │      with pd->file->*         │
│  └─────┬────────────┘                                │
│        │                                              │
│  ┌─────▼────┐                                        │
│  │ Playdate │  /vgm/*.adx, *.dsp, *.vag, ...        │
│  │ Flash FS │                                        │
│  └──────────┘                                        │
│                                                       │
│  ┌──────────────────────────────────────────────┐   │
│  │              UI (1-bit graphics)              │   │
│  │  File Browser → Player (progress, waveform)  │   │
│  │  D-pad: navigate/seek  A: play  B: stop      │   │
│  │  Crank: volume control                        │   │
│  └──────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
```

## Data Flow

1. **File Browser** scans `/vgm/` on the Playdate filesystem for supported extensions
2. User selects a file → `player_open_file()` creates a vgmstream instance
3. **vgm_pd_streamfile** adapter lets vgmstream read the file via Playdate's `pd->file->*` API
4. vgmstream detects the format, parses the header, initializes the decoder
5. `addSource()` registers our **audio callback** with the Playdate sound engine
6. Every audio render cycle (~5.8ms at 44.1kHz):
   - Callback checks if the decode ring buffer has samples
   - If empty, calls `libvgmstream_fill()` to decode a chunk
   - Deinterleaves stereo and applies crank-based volume
   - Fills the Playdate's `left`/`right` output buffers

## Supported Formats (subset safe for Playdate)

These formats use lightweight codecs (ADPCM variants, simple PCM) that the Cortex-M7 can handle:

| Extension | Format | Notes |
|-----------|--------|-------|
| `.adx` | CRI ADX | Very common in Japanese games, ADPCM-based |
| `.dsp` | Nintendo DSP ADPCM | GC/Wii standard, lightweight |
| `.brstm` | BRSTM | Wii/Revolution stream |
| `.bcstm` | BCSTM | 3DS stream |
| `.bfstm` | BFSTM | Switch/WiiU stream |
| `.vag` | PS2 VAG | PlayStation ADPCM |
| `.ss2` | PS2 SS2 | Sony stream |
| `.ads` | PS2 ADS | Sony headerless |
| `.idsp` | Interleaved DSP | Common in GC/Wii games |
| `.str` | Various STR | Multiple game engines |
| `.txtp` | vgmstream TXTP | Text-based playlist/config |

Formats requiring heavy CPU (Vorbis, AAC, ATRAC9, MPEG) are disabled in the CMake config.

## Building

### Prerequisites

- [Playdate SDK](https://play.date/dev/) (3.x+)
- CMake 3.20+
- ARM GCC toolchain (for device builds; included with Playdate SDK)

### Setup

```bash
# Clone this project
git clone <this-repo>
cd playdate-vgm-player

# Clone vgmstream into the project
git clone https://github.com/vgmstream/vgmstream.git

# Set your SDK path
export PLAYDATE_SDK_PATH=/path/to/PlaydateSDK
```

### Build for Simulator

```bash
mkdir build-sim && cd build-sim
cmake .. -DCMAKE_BUILD_TYPE=Debug
make
```

### Build for Device

```bash
mkdir build-device && cd build-device
cmake .. -DCMAKE_TOOLCHAIN_FILE=$PLAYDATE_SDK_PATH/C_API/buildsupport/arm.cmake
make
```

### Install Audio Files

Copy game audio files into the `.pdx` bundle:

```bash
mkdir -p YourGame.pdx/vgm/
cp ~/your-game-rips/*.adx YourGame.pdx/vgm/
cp ~/your-game-rips/*.dsp YourGame.pdx/vgm/
```

## Controls

| Input | Browser | Player |
|-------|---------|--------|
| D-pad Up/Down | Navigate files | — |
| D-pad Left/Right | — | Seek ±5s |
| A button | Open & Play | Play/Pause |
| B button | — | Stop → Browser |
| Crank | — | Volume |

## Project Structure

```
playdate-vgm-player/
├── CMakeLists.txt              # Build system
├── README.md                   # This file
├── Source/
│   └── pdxinfo                 # Playdate metadata
├── src/
│   ├── main.c                  # Game loop, UI, audio callback
│   └── vgm_pd_streamfile.c    # Playdate FS ↔ vgmstream adapter
└── vgmstream/                  # (clone here)
```

## Key Implementation Notes

### Audio Callback Bridge

The central challenge is bridging vgmstream's pull-based decoding with Playdate's push-based audio callback. vgmstream decodes on demand (you call `libvgmstream_fill()`), while Playdate calls your callback when it needs audio. The solution is a small ring buffer:

```
vgmstream_fill() → [ring buffer] → audio_callback() → speaker
     (decode)       (512 samples)     (Playdate calls)
```

### Streamfile Adapter

vgmstream abstracts all file I/O through "streamfiles". This POC implements the `libstreamfile` interface by wrapping Playdate's `SDFile` operations (`open`, `read`, `seek`, `close`). This is the key integration point — without it, vgmstream can't read anything.

### Memory Considerations

- vgmstream allocates memory for its decoder state (typically 1-100KB depending on codec)
- The decode buffer is small (512 × 2 × 2 bytes = 2KB)
- Total RAM usage should stay well under 1MB for most formats
- Playdate has 16MB total, with ~10MB typically available to games

### CPU Budget

- Playdate runs at 180MHz with a hardware FPU
- ADPCM codecs (ADX, DSP, VAG) decode very efficiently — typically <1ms per 512-sample block
- The audio callback must complete in <5.8ms (256 samples at 44.1kHz)
- Heavy codecs (Vorbis, AAC) are disabled to avoid overruns

## Limitations & Future Work

- **POC only** — needs actual compilation testing with vgmstream's build system
- **No resampling** — files must be at 44100Hz or you'll hear pitch changes (vgmstream's internal resampler could be enabled but costs CPU)
- **Limited format support** — only ADPCM-class codecs enabled
- **No loop visualization** — vgmstream supports loop points but they're not shown in UI
- **No playlist** — single-file playback only (TXTP files could chain tracks)
- **No subsong support** — some formats (FSB, BNK) contain multiple subsongs

## License

This POC code is MIT. vgmstream itself is ISC-licensed.
