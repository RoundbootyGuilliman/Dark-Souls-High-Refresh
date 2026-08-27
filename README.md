# Dark Souls High Refresh

This mod enables high refresh rate and high FPS in Dark Souls Remastered while preserving the original game speed.

## Installation

1. Download `Dark-Souls-High-Refresh-v1.4.zip` from the [releases](https://github.com/RoundbootyGuilliman/Dark-Souls-High-Refresh/releases) section.
2. Extract its contents into the Dark Souls Remastered directory beside `DarkSoulsRemastered.exe`.
3. Open `DSHRHook.ini` and set `TargetFPS` to the desired frame rate. For the smoothest VRR experience it is recommended to keep this a few frames below your screen refresh rate (e.g. 162 for a 165hz screen).
4. Start the game normally.
5. Set the in-game setting `System -> PC Settings -> Display -> Frequency` to the refresh rate of your monitor (not target FPS). So if you have a 165hz monitor, set it to `165hz`. Set the `Vertical sync` to `OFF`.

If the game directory already contains a different `dinput8.dll`, do not overwrite it. Another proxy-based mod will require a compatibility solution before both can be used together.

Example configuration:

```ini
[DSHR]
TargetFPS=162
```

`TargetFPS` accepts any integer number from 61 through 1000. Common choices include 117, 141, 162, and 237. Restart the game after changing it.

Driver-level frame caps, V-sync, display refresh, or insufficient performance can prevent the game from reaching the configured target. Sustained performance below the target WILL produce slow motion, choose the target that your PC can consistently muster.

## What it adds/fixes

- Configurable high-refresh rendering and frame pacing.
- Gameplay, movement, physics, and animation speed.
- Particle and visual-effect timing, like flames, dust, and impact sparks.
- FromSoftware's Remo in-engine cinematics.
- (v1.1) Havok cloth and rigid-body physics timing.
- (v1.2) Launcher-free proxy loading with no cross-process injection.
- (v1.3) Support for the current November 2022 Steam executable.
- (v1.4) Bug fixes: mouse sensitivity, controller menu navigation, and in-engine cinematics flickering.

## Uninstallation

Close the game before uninstalling.

- v1.2 and later: Remove `dinput8.dll`, `DSHRHook.ini`, and `DSHRHook.log` from the game directory.
- v1.0 / v1.1: Remove `DSHRLauncher.exe`, `DSHRHook.dll`, `DSHRHook.ini`, `DSHRHook.log`, and `DSHR.log` from the game directory.

## Tested with

- Dark Souls Remastered PC patch `1.03` and `1.03.1` with either supported executable:
  - 2018 build SHA-1 `F0ECFBE20D780751248DFB2A9759D6215E246676`.
  - 2022 Steam build SHA-1 `9150CC63C617332ED3C2C66E7566ED67E3292DA0`.
- Sustained gameplay and visual effects at 117 FPS / 120hz, and 162 FPS / 165hz.

Only the exact executable builds identified above are supported.
Multiplayer compatibility has not been validated; keep the game offline while using the mod. Back up irreplaceable saves.

## Bug reporting

If you encounter bugs, please let me know via the [issues](https://github.com/RoundbootyGuilliman/Dark-Souls-High-Refresh/issues) section. Include the save file so i can see and debug the issue right away.

## License

Released under the [Unlicense](LICENSE).
