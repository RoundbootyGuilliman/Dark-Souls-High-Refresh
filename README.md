# Dark Souls High Refresh

This mod enables high refresh rate and high FPS in Dark Souls Remastered while preserving the original game speed.

It uses the dormant `FrpgFlipper_140FPS` scheduler included in the game and corrects the timing of rendering, gameplay, visual effects, and in-engine cinematics. Prerecorded movies are left untouched.

## Installation

1. Download `Dark-Souls-High-Refresh-v1.0.zip` from the [releases](https://github.com/RoundbootyGuilliman/Dark-Souls-High-Refresh/releases) section.
2. Extract its contents into the Dark Souls Remastered directory beside `DarkSoulsRemastered.exe`.
3. Open `DSHRHook.ini` and set `TargetFPS` to the desired frame rate. The most stable and bug-free value seems to be 120, but i plan to iron out the bugs in the future to better support more custom values.
4. Start the game with `DSHRLauncher.exe`.
5. Set the in-game setting `System -> PC Settings -> Display -> Frequency` to the refresh rate of your monitor (not target FPS). So if you have a 165hz monitor, set it to `165hz`. Set the `Vertical sync` to `OFF`.

Example configuration:

```ini
[DSHR]
TargetFPS=120
```

`TargetFPS` accepts any integer number from 61 through 1000. The recommended value is 120. Restart the game after changing it.

Driver-level frame caps, V-sync, display refresh, or insufficient performance can prevent the game from reaching the configured target. Sustained performance below the target WILL produce slow motion, choose the target that your PC can consistently muster.

## What it adds/fixes

- Configurable high-refresh rendering and frame pacing.
- Gameplay, movement, physics, and animation speed.
- Particle and visual-effect timing, like flames, dust, and impact sparks.
- FromSoftware's Remo in-engine cinematics.

## Uninstallation

Close the game and remove `DSHRLauncher.exe`, `DSHRHook.dll`, `DSHRHook.ini`, `DSHRHook.log`, and `DSHR.log` from the game directory.

The mod does not alter the game executable, saves, or packaged game assets. Launching `DarkSoulsRemastered.exe` normally also bypasses the mod.

## Tested with

- Dark Souls Remastered PC patch `1.03`.
- `DarkSoulsRemastered.exe` SHA-1 `F0ECFBE20D780751248DFB2A9759D6215E246676`.
- Sustained gameplay and visual effects at 120 FPS / 165hz.

Only the exact executable build identified above is supported.
Multiplayer compatibility has not been validated; it is recommended to keep the game offline while testing. Back up irreplaceable saves.

## Bug reporting

As the game required separate and specific patching for several different game systems, namely the rendering, the gameplay simulation, the VFX and the in-engine cinematics, there could be something that i missed, and that will look or behave weird.
If you encounter that, please let me know via the [issues](https://github.com/RoundbootyGuilliman/Dark-Souls-High-Refresh/issues) section. Include the save file so i can see and debug the issue right away.

## License

Released under the [Unlicense](LICENSE).
