# Radio Music

![](https://github.com/maetyu-d/radio_music/blob/main/Screenshot%202026-03-04%20at%2021.36.23.png)

A JUCE instrument plugin that:
- picks internet radio streams from a built-in station list,
- continuously preloads and randomly switches between stations,
- can crossfade between streams without intentional gaps,
- applies stutter, granular, micro-loop, and or wild resampling effects.

## Build

### 1) Provide JUCE
Use either:
- `-DJUCE_DIR=/absolute/path/to/JUCE` (JUCE repo root containing `CMakeLists.txt`), or
- copy JUCE into `./JUCE` next to this project.

### 2) Configure
```bash
cmake -S . -B build -DJUCE_DIR=/absolute/path/to/JUCE
```

### 3) Build
```bash
cmake --build build --config Release
```

Targets are set for `AU`, `VST3`, and `Standalone`.

## Run Standalone (macOS)
Launch the `.app` bundle (do not run the raw `Contents/MacOS/...` binary directly):

```bash
open "/Users/md/Downloads/radio JUCE plugin/build/RandomRadioFX_artefacts/Standalone/Radio Music.app"
```

If you want to run from Finder, open:
`/Users/md/Downloads/radio JUCE plugin/build/RandomRadioFX_artefacts/Standalone/Radio Music.app`

## Notes
- Internet radio compatibility depends on stream codec support in your JUCE build.
- Included station URLs are public examples and may change over time.
- Crossfades are block-accurate and the next station is preloaded on the inactive deck.
