<h1 align="center">Nifer's Multi-Pass Tool (MPT)</h1>
<p align="center">
  <a href="https://github.com/ItsNifer/Nifer-MultiPass/releases/latest"><img alt="Download" src="https://custom-icon-badges.demolab.com/badge/-Download-limegreen?style=for-the-badge&logo=download&logoColor=white"></a>
  <a href="https://twitter.com/NiferEdits"><img alt="Twitter" src="https://img.shields.io/badge/twitter-1DA1F2?style=for-the-badge&logo=twitter&logoColor=white"></a>
</p>
<h5 align="center">Nifer's Multi-Pass Tool is a ReShade shader, ReShade addon and OBS plugin that record multiple render passes from your game at the same time. Color (without shaders), Color (with shaders), Depth and Normals are each captured at full resolution and recorded to their own separate video file, in sync, from a single button press. Depth can also be exported as a 32-bit float EXR image sequence for compositing.                                                                               </p>
  <p align="center">
  <img src="https://github.com/ItsNifer/Multi-Pass-Tool/blob/main/img/MPT_example.png" alt="MPT"/>
</p>
</h5>

## Passes
| Pass | Contains |
| --- | --- |
| **Color (without shaders)** | Clean gameplay before any ReShade shaders, including the game UI |
| **Color (with shaders)** | Gameplay after your ReShade shaders, exactly as seen on screen, without the ReShade menu |
| **Depth** | Grayscale depth, no game UI. Also exportable as 32-bit float EXR |
| **Normals** | Screen space surface normals, no game UI |

## Usage
1. Download the [latest](https://github.com/ItsNifer/Multi-Pass-Tool/releases/latest) update from the releases page.
2. Install ReShade 6.7+ into your game with **full add-on support** (only use on offline clients / singleplayer games).
3. Extract "**NiferMultiPass.fx**" into "(game directory)/reshade-shaders/Shaders"
4. Extract "**nifer_multipass.addon64**" into the game directory (where you see the .exe of the game)
5. Extract "**nifer-multipass-source.dll**" into "(OBS folder)/obs-plugins/64bit", then restart OBS
6. Once in game, open the ReShade menu and order the effect list like this:
   - "**NiferMultiPass**" at the very **TOP** of the list
   - "**NiferMultiPassColor**" at the very **BOTTOM** of the list (only needed for the Color with shaders pass)
7. Both techniques draw nothing on screen, that is correct. Your game looks completely normal while recording.

<p align="left">
  <img src="https://github.com/ItsNifer/Multi-Pass-Tool/blob/main/img/MP_reshade.png" alt="reshade list"/>
</p>

## OBS Setup
**To add the passes into OBS:**
1. Add source > "**Reshade Pass Capture (Nifer)**" and pick a Pass from the dropdown
2. Repeat for every pass you want, one source each
3. In each source, tick "**Record this pass**" and set a Recording folder
4. Encoder and Frame rate can be left on "**Use OBS recording settings**", which copies your normal OBS recording setup, or set per pass
5. Press Start Recording. Every pass writes its own file, all starting and stopping on the same frame

<p align="left">
  <img src="https://github.com/ItsNifer/Multi-Pass-Tool/blob/main/img/MP_obs.png" alt="obs sources"/>
</p>

## Hotkeys
**To record without the OBS record button:**
1. Go into Settings > Hotkeys
2. Search for "**Record all Multi Pass sources**" to start and stop every pass at once
3. Each source also has its own "**Record this Multi Pass source**" hotkey, listed under the source's name
4. A high beep means recording started, a low beep means it stopped

Hidden sources are never recorded, so hide a pass with the eye icon to leave it out of the next recording.

## EXR Depth Export
**For full precision depth instead of 8-bit video:**
1. Select the Depth source and tick "**Export as EXR image sequence**"
2. Recording writes one 32-bit float exr per game frame (channel Z, linear depth)
3. Files are large, roughly 8 MB per 1080p frame, so use a fast SSD
4. Lower "**Capture FPS Limit**" in the shader settings to reduce the load

## No Add-On Version
If you cannot install ReShade with add-on support, the **Grid** version works with any ReShade build. The shader draws the passes on screen and the OBS plugin crops each one into its own source. Choose a layout depending on what matters more to you:

| Layout | Trade off |
| --- | --- |
| **Four passes (2x2)** | All four at once, each a quarter of the screen |
| **Two passes (side by side)** | Full vertical resolution, half horizontal |
| **One pass (fullscreen)** | Native resolution, one pass at a time |
| **Frame cycle** | Native resolution, pass frame rate becomes game fps / cycle length |

## Requirements
- ReShade 6.7+ with full add-on support (the Grid version works on any build)
- A DirectX 10, 11 or 12 game (Vulkan and OpenGL are not supported)
- OBS Studio 28 or newer, 64-bit

## Support

For support and bug reporting, either submit and issue under the github repo - or personally DM me on twitter @NiferEdits with the issue.

## Credits
- **Crosire** - ReShade
- **Hugh Bailey, Patrick Mours, Crosire** - obs_capture addon example this method is based on
- **ReShade team** - DisplayDepth.fx (normals reconstruction)
