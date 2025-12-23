# CSE2 Portable - Nintendo GameCube Port

This is a port of CSE2 (Cave Story Decompilation) to the Nintendo GameCube using devkitPro.

## Requirements

- devkitPro with devkitPPC
- libogc
- libfat (for SD card access via SD adapter)
- libaesnd (for audio)

## Building

### Using the Makefile (Recommended)

1. Set up your devkitPro environment:
   ```bash
   export DEVKITPRO=/opt/devkitpro
   export DEVKITPPC=$DEVKITPRO/devkitPPC
   export PATH=$DEVKITPPC/bin:$PATH
   ```

2. Build the project:
   ```bash
   make -f Makefile.gc
   ```

3. The output will be `CSE2GC.dol` which can be run on a GameCube or in Dolphin Emulator.

### Build Options

Edit `Makefile.gc` to enable/disable options:

- **Japanese Build**: Uncomment `-DJAPANESE` in CFLAGS/CXXFLAGS
- **Bug Fixes**: `-DFIX_MAJOR_BUGS` is enabled by default

### Clean Build

```bash
make -f Makefile.gc clean
```

## Running

### On Dolphin Emulator

```bash
make -f Makefile.gc run
```

Or manually:
```bash
dolphin-emu -e CSE2GC.dol
```

### On Real Hardware

You'll need:
- A way to load homebrew (Swiss, Action Replay, etc.)
- An SD card adapter for loading game data and saves
- The game data files

## SD Card Adapters

The following SD card adapters are supported (in order of detection priority):

1. **SD Gecko Slot B** (memory card slot B) - Most common setup
2. **SD Gecko Slot A** (memory card slot A) - Fallback
3. **SD2SP2** (Serial Port 2) - Last resort

The game will automatically detect and use the first available adapter.

## Data Files

Place the Cave Story `data` folder in one of these locations:

1. **SD Card** (requires SD adapter):
   - English: `sd:/cse2/data/`
   - Japanese: `sd:/cse2-jp/data/`

2. **Embedded** (default):
   - Game data is embedded into the DOL using bin2h
   - No SD card needed for gameplay, but saves require SD card

## Save Files

Save files (`Profile.dat`) are stored on the SD card:
- English: `sd:/cse2/Profile.dat`
- Japanese: `sd:/cse2-jp/Profile.dat`

**Note:** If no SD card is detected, saving will be disabled. The game will show a warning when attempting to save without an SD card.

## Controls

| GameCube Button | Action |
|-----------------|--------|
| A | Jump |
| B | Shoot |
| X | Inventory |
| Y | Map |
| L Trigger | Previous Weapon |
| R Trigger | Next Weapon |
| D-Pad / Analog | Movement |
| Start | Menu/Escape |
| Z | Settings (F2) |

## Technical Details

### Backends Used

- **Platform**: GameCube (PAD input, timing, filesystem)
- **Renderer**: Software (with GX/VI framebuffer output)
- **Audio**: Software Mixer with AESND

### Resolution

The game renders at its native 320x240 resolution and is scaled to fit the TV output (typically 640x480).

### Memory

The GameCube has 24MB of main RAM and 16MB of auxiliary RAM. CSE2 should fit comfortably with the software renderer.

## Known Limitations

1. **No Native Memory Card Support** - Save files use SD card via SD Gecko/SD2SP2
2. **No DVD Loading** - Game data is embedded or loaded from SD card
3. **GX Hardware Renderer** - Uses GX for 2D rendering with proper cache synchronization

## File Structure

```
CSE2-GC/
├── Makefile.gc              # GameCube build makefile
├── src/
│   └── Backends/
│       ├── Platform/
│       │   └── GameCube.cpp     # Platform backend
│       ├── Rendering/
│       │   └── Window/
│       │       └── Software/
│       │           └── GameCube.cpp  # Framebuffer backend
│       └── Audio/
│           └── SoftwareMixer/
│               └── GameCube.cpp      # Audio backend
└── README_GAMECUBE.md       # This file
```

## Troubleshooting

### Black Screen
- Ensure data files are in the correct location (embedded or on SD card)
- Check that your SD adapter is working

### No Audio
- Audio uses AESND library - ensure it's properly linked
- Check Dolphin audio settings if testing in emulator

### Controls Not Working
- Make sure a controller is connected to port 1
- PAD_Init() must be called before input works

### Save Not Working
- Ensure your SD Gecko is properly inserted (Slot B recommended)
- Check that the SD card is formatted as FAT32
- The game will create the `sd:/cse2/` directory automatically
- Check Dolphin's log for "SD Gecko Slot B mounted successfully!"

### Tile/Graphics Corruption
- This has been fixed with proper GX cache synchronization
- If you still see issues, ensure you're using the latest build

## Credits

- Original Cave Story by Daisuke "Pixel" Amaya
- CSE2 Decompilation by Clownacy
- GameCube port based on CSE2 Portable and CSE2-Wii

## License

See the main LICENSE.txt file. The GameCube backend code is released under the MIT license.

