# OpenJK

OpenJK is a community effort to maintain and improve the game and engine powering Jedi Academy and Jedi Outcast, while maintaining _full backwards compatibility_ with the existing games and mods.  
This project does not intend to add major features, rebalance, or otherwise modify core gameplay.

Our aims are to:

- Improve the stability of the engine by fixing bugs and improving performance.
- Support more hardware (x86_64, Arm, Apple Silicon) and software platforms (Linux, macOS)
- Provide a clean base from which new code modifications can be made.

[![discord](https://img.shields.io/badge/discord-join-7289DA.svg?logo=discord&longCache=true&style=flat)](https://discord.gg/dPNCfeQ)
[![forum](https://img.shields.io/badge/forum-JKHub.org%20OpenJK-brightgreen.svg)](https://jkhub.org/forums/forum/49-openjk/)

[![build](https://github.com/JACoders/OpenJK/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/JACoders/OpenJK/actions/workflows/build.yml?query=branch%3Amaster)
[![coverity](https://scan.coverity.com/projects/1153/badge.svg)](https://scan.coverity.com/projects/1153)

## Supported Games

| Game | Single Player | Multi Player |
| - | - | - |
| Jedi Academy | ✅ Stable | ✅ Stable |
| Jedi Outcast | 😧 Works, needs attention | 🙅 Not supported - consider [JK2MV](https://jk2mv.org) |

Please direct support queries, discussions and feature requests to the JKHub sub-forum or Discord linked above.

## Settings added in this fork

This fork (single player) adds the console variables below. Type them in the console (`~`) as `name value`, or put them in your config. Unless noted, they are saved in your config and take effect immediately. "Latched" ones take effect after `vid_restart`.

### Launcher

The single player program opens a chooser to pick which game to play. It reads both games' install folders from `campaigns.ini`, next to the program. To skip the chooser, pass one of these on the command line:

| Argument | What it does |
| - | - |
| `--campaign=academy` | Start Jedi Academy. Jedi Outcast's game data is mounted too when `campaigns.ini` knows where it is. |
| `--campaign=outcast` | Start Jedi Outcast on its own engine. |
| `--campaign=outcast-ja` | Start Jedi Academy with Jedi Outcast's story already chosen. |

### Movement

| Setting | Default | What it does |
| - | - | - |
| `g_momentumMovement` | `1` | Momentum-based movement for the player: speeding up, braking and turning take time. `0` gives back the original movement exactly. |
| `g_momentumAccelerationTime` | `0.25` | Seconds to go from standing still to full running speed. |
| `g_momentumStopTime` | `0.25` | Seconds to brake from full speed to a stop after letting go of the keys. |
| `g_momentumTurnTime` | `0.12` | Seconds for the movement to swing most of the way toward a new direction. Smaller turns sharper; larger feels heavier and skids more. |
| `g_momentumOverspeedDecayTime` | `0.6` | Seconds to lose a full running speed's worth of extra speed, for example after landing from a fast jump. |
| `g_momentumAirControl` | `1.0` | Steering in the air. `1` = original, `0` = none. |

### Lightsaber

| Setting | Default | What it does |
| - | - | - |
| `g_saberGuard` | `1` | The player blocks only while holding alt-attack (right mouse) with the saber out. Each block costs Force, less with higher Saber Defense. Turning the view aims the guard; a strike from the side it faces is blocked for free. Raising the guard just before a strike lands parries it and can disarm the attacker. The saber throw moves to its own key: `bind CTRL +button10`. `0` = the original automatic blocking. `2` also prints every block to the console. |
| `g_saberReach` | `1` | The saber arm reaches for the blaster bolts it deflects. `0` = the animation alone. `2` also prints every reach. `3` (testing) holds each pose until the next bolt. |
| `g_saberSlicing` | `1` | Lightsaber blows cut characters into pieces where the blade passes. `0` = the game's own dismemberment. |

### Ragdolls

Characters become physics ragdolls when they die. The muscle-driven behaviours are off by default; turn them on with the settings below.

| Setting | Default | What it does |
| - | - | - |
| `g_ragdoll` | `1` | `1` = characters become ragdolls when they die. `0` = the canned death animations. |
| `g_ragdollKnockdowns` | `1` | Characters knocked down by Force Push or Pull fall as ragdolls and get back up. |
| `g_ragdollImpulseScale` | `1.0` | How hard hits push bodies around. |
| `g_ragdollMuscleStrength` | `0` | Strength of the simulated muscles. `0` = always limp. `1` = the body staggers, reaches out as it falls and keeps its head up. `2` = twice as strong. |
| `g_ragdollWritheChance` | `0` | Chance (0 to 1) that a dying character moves weakly on the ground before going limp. |
| `g_ragdollHitReactions` | `0` | `1` = living characters flinch physically when hit, while they keep fighting. |
| `g_ragdollHitReactionScale` | `0.4` | How hard hits jolt living characters, compared with how hard they push corpses. |
| `g_ragdollShotReactions` | `0` | `1` = a hit takes control of an NPC: its AI pauses while its body reacts, clutches the wound, staggers and recovers. Bosses and saber users only flinch. |
| `g_ragdollStumbles` | `0` | `1` = hits add up and can make NPCs stumble and fall over. |
| `g_ragdollBalance` | `1300` | How much push a character can take in a short time before it stumbles. Falling over takes about twice as much. Bigger is steadier. |
| `g_ragdollDebug` | `0` | `1` = print each ragdoll's behaviour changes to the console. Not saved. |

### Jedi Outcast on the Jedi Academy engine

| Setting | Default | What it does |
| - | - | - |
| `fs_outcastpath` | (empty) | Jedi Outcast's `GameData` folder. Set by the launcher from `campaigns.ini`. With it, New Game offers Jedi Outcast's story. |
| `g_campaign` | `academy` | Which story is being played: `academy` or `outcast`. Set by New Game and kept in saved games. |
| `ui_style` | `academy` | Whose menus and HUD to use: `academy` or `outcast` (needs Jedi Outcast's game data). Also in Setup → Options as "Interface". New Game and loading a saved game switch it to that story's own. |

### Graphics

Rows for most of these are in Setup → More Video, and appear only when your renderer can do the effect.

| Setting | Default | What it does |
| - | - | - |
| `r_glsl` | `1` | Use shader programs where the renderer has them. The effects below that need shaders depend on it. Latched. |
| `r_ssao` | `0` | `1` = ambient occlusion: soft shading in corners and where things meet. |
| `r_ssaoRadius` | `48` | How far, in game units, a surface still shades its surroundings. |
| `r_ssaoStrength` | `2` | How dark the ambient occlusion is. |
| `r_bloom` | `0` | `1` = a soft glow around the brightest parts of the picture. |
| `r_bloomThreshold` | `0.8` | How bright (0 to 1) a pixel must be to glow. |
| `r_bloomIntensity` | `0.5` | How strong the glow is. |
| `r_smaa` | `0` | `1` = SMAA: smooths edges without blurring textures. Works with or without anti-aliasing. |
| `r_sharpen` | `0` | Sharpening of the 3D view, from `0` (off) to `1`. |
| `r_lightShafts` | `0` | Rays of sunlight through gaps and around edges, on levels with a sun: `0` off, `1` normal, up to `2`. |
| `r_eyeAdaptation` | `0` | The view brightens in dark places and dims in bright ones, over a second or two: `0` off, `1` normal, up to `2`. |
| `r_dlightGLSL` | `0` | Dynamic lights (sabers, blasters, explosions) follow the shape of what they fall on. `1` = on, `2` = also the bumps of the surface. |
| `r_lampLighting` | `1` | Characters lit by the level's own lamps, each from its side. `0` off, `1` with ray tracing, `2` always. |
| `r_softParticles` | `1` | Smoke, sparks and glows fade where they meet walls and floors instead of ending in a hard line (Vulkan renderer). |
| `r_smoothLightmaps` | `1` | Level lighting is smoothed with a bicubic filter as the level loads. Latched. |
| `r_alphaToCoverage` | `1` | Smooths the edges of see-through textures (leaves, fences) when anti-aliasing is on. |
| `r_sampleShading` | `0` | `0` = off. Up to `1` = shade every anti-aliasing sample. Smoother, but slow. Latched. |
| `r_surfaceSpriteRange` | `3` | Multiplies how far away grass, bushes and the like are still drawn. |
| `r_wideUI` | `1` | On wide screens, menus and the HUD keep their shape instead of being stretched. |

Shadows. Setup's shadow row also offers Soft shadows (`cg_shadows 4`) where the renderer can draw them.

| Setting | Default | What it does |
| - | - | - |
| `r_shadowSoftness` | `1` | Width of the soft edge of volumetric shadows. `0` = hard edges. |
| `r_shadowSamples` | `8` | How many shadow volumes make up one soft shadow. |
| `r_shadowStrength` | `1` | How dark shadows are. |
| `r_shadowTilt` | `1` | How much shadows lean away from the light. |
| `r_shadowSelf` | `0` | `1` = characters are darkened by shadows too. |
| `r_shadowMaps` | `0` | `1` = Volumetric shadows (`cg_shadows 2`) also draw shadow maps, as Soft shadows do. |
| `r_rtLamps` | `1` | Ray traced character shadows fall away from the level's lamps. `0` = from the level's single average light direction. |

### Vulkan renderer

The Vulkan renderer is the default (`cl_renderer rdsp-vulkan`). Set `cl_renderer rdsp-vanilla` for OpenGL. The game falls back to OpenGL by itself when Vulkan can't be used.

| Setting | Default | What it does |
| - | - | - |
| `r_rayTracing` | `0` | Ray tracing: exact character shadows, and the light of sabers, blasters and explosions stops at walls. `0` off, `1` medium, `2` high (smoother shadows, slower). Needs a GPU with ray tracing. |
| `r_vkRayTracing` | `1` | `0` = never turn ray tracing on, even where the GPU has it. Latched. |
| `r_vkDevice` | `-1` | Which GPU to use. `-1` picks the best one (a dedicated card). Otherwise, an index in the list printed at start-up. Latched. |
| `r_lowLatency` | `1` | `1` = shorter input lag: each frame's input is read as late as possible. `0` = frames are queued as far as the driver allows. |
| `r_adaptiveVsync` | `1` | With vsync on (`r_swapInterval 1`), a late frame tears instead of waiting a whole refresh. |
| `r_vkValidation` | `0` | `1` = turn on Vulkan's validation layers, for debugging. Latched. |

### Performance

| Setting | Default | What it does |
| - | - | - |
| `com_jobThreads` | `-1` | Worker threads for loading and other parallel work. `-1` = use what the processor has. `0` = no extra threads. |

### Debugging

These are not saved.

| Setting | What it does |
| - | - |
| `r_postDebug` | `1` = run the post-processing steps without any effect: the picture must not change. |
| `r_rtDebug` | `1` = darken the view where rays to the sun are blocked. `2` = show only that. |
| `r_vkProfile` | `1` = report where the renderer waits for the GPU, when it waits long. |

### Set by the game

These report state and are not meant to be changed:

- `r_glslAvail`, `r_postAvail`, `r_smaaAvail`, `r_softAvail`, `r_shadowMapsAvail`, `r_rtAvail`: whether the renderer can do each effect. They decide which rows Setup shows.
- `ui_outcastInterface`: `1` while Jedi Outcast's menus are loaded.
- `cg_missionstatusscreen`: `1` when the next loading screen is to show the statistics of the level just finished (Jedi Outcast's level changes ask for it; its menus show them).
- `cl_rendererFailure`: why the Vulkan renderer couldn't start, if it couldn't.
- `com_configVersion`: how far an old config has been brought up to date.
- `com_levelLoading`, `r_outcastAnimationsStart`: internal.
- `ui_r_resolution`, `ui_r_ext_multisample`: the Video page's choices of resolution and anti-aliasing, applied with Apply.

### Changed defaults

| Setting | Upstream | Here |
| - | - | - |
| `cl_renderer` | `rdsp-vanilla` | `rdsp-vulkan` |
| `cg_fovAspectAdjust` | `0` | `1` |
| `r_shadowRange` | `1000` | `2000` |
| `r_lodscale` | `10` | `20` |
| `r_simpleMipMaps` | `1` | `2` (Lanczos filtering for the smaller texture levels; `0` blur, `1` box) |

## License

OpenJK is licensed under GPLv2 as free software. You are free to use, modify and redistribute OpenJK following the terms in [LICENSE.txt](https://github.com/JACoders/OpenJK/blob/master/LICENSE.txt)

## For players

To install OpenJK, you will first need Jedi Academy installed. If you don't already own the game you can buy it from online stores such as [Steam](https://store.steampowered.com/app/6020/), [Amazon](https://www.amazon.com/Star-Wars-Jedi-Knight-Academy-Pc/dp/B0000A2MCN) or [GOG](https://www.gog.com/game/star_wars_jedi_knight_jedi_academy).

Download the [latest build](https://github.com/JACoders/OpenJK/releases/tag/latest) ([alt link](https://builds.openjk.org)) for your operating system.

Installing and running OpenJK:

1. Copy everything from your install/JediAcademy folder except campaigns.ini into the game's GameData folder, the one that contains base. For Steam that's <Steam folder>\steamapps\common\Jedi Academy\GameData\. Nothing of the original game gets overwritten, because all the file names are different.
2. Start openjk_sp.x86_64.exe. Steam's Play button still starts the original game, so a desktop shortcut to this exe is easiest.
3. On first launch it creates a fresh campaigns.ini next to the exe. Jedi Academy works without editing it.

Installing with Jedi Outcast:

1. Open that campaigns.ini and set JediOutcastGameData = to Jedi Outcast's GameData folder, for example C:\Program Files (x86)\Steam\steamapps\common\Jedi Outcast\GameData.
2. Then choose the story at New Game. It runs on the Jedi Academy engine, so nothing else is needed.
3. For the original JO engine as well: copy install/JediOutcast into Jedi Outcast's GameData folder, and set JediOutcastEngine = to the openjo_sp.x86_64.exe there.

### Linux Instructions

If you do not have an existing JKA installation and need to download the base game:

1. Download and Install SteamCMD [SteamCMD](https://developer.valvesoftware.com/wiki/SteamCMD#Linux).
1. Set the download path using steamCMD: `force_install_dir /path/to/install/jka/`
1. Using SteamCMD Set the platform to windows to download any windows game on steam. @sSteamCmdForcePlatformType "windows"
1. Using SteamCMD download the game, `app_update 6020`.

Extract the contents of the file into the Jedi Academy `GameData/` folder. For Steam users, this will be in `<Steam Folder>/steamapps/common/Jedi Academy/GameData/`.

### macOS Instructions

If you have the Mac App Store Version of Jedi Academy, follow these steps to get OpenJK runnning under macOS:

1. Install [Homebrew](https://brew.sh/) if you don't have it.
1. Open the Terminal app, and enter the command `brew install sdl2`.
1. Extract the contents of the OpenJK DMG into the game directory `/Applications/Star Wars Jedi Knight: Jedi Academy.app/Contents/`
1. Run `OpenJK.app` or `OpenJK SP.app`
1. Savegames, Config Files and Log Files are stored in `/Users/$USER/Library/Application Support/OpenJK/`

## For Developers

### Building OpenJK

- [Compilation guide](https://github.com/JACoders/OpenJK/wiki/Compilation-guide)
- [Debugging guide](https://github.com/JACoders/OpenJK/wiki/Debugging)

### Contributing to OpenJK

- [Fork](https://github.com/JACoders/OpenJK/fork) the project on GitHub
- Create a new branch and make your changes
- Send a [pull request](https://help.github.com/articles/creating-a-pull-request) to upstream (JACoders/OpenJK)

### Using OpenJK as a base for a new mod

- [Fork](https://github.com/JACoders/OpenJK/fork) the project on GitHub
- Change the JK_VERSION define in codemp/qcommon/game_version.h from "OpenJK" to your project name
- If you make a nice change, please consider back-porting to upstream via pull request as described above. This is so everyone benefits without having to reinvent the wheel for every project.

## Maintainers (full list: [@JACoders](https://github.com/orgs/JACoders/people))

Leads:

- [Ensiform](https://github.com/ensiform)
- [razor](https://github.com/Razish)
- [Xycaleth](https://github.com/xycaleth)

## Significant contributors ([full list](https://github.com/JACoders/OpenJK/graphs/contributors))

- [bibendovsky](https://github.com/bibendovsky) (save games, platform support)
- [BobaFett](https://github.com/Lrns123)
- [BSzili](https://github.com/BSzili) (JK2, platform support)
- [Cat](https://github.com/deepy) (infra)
- [Didz](https://github.com/dionrhys)
- [eezstreet](https://github.com/eezstreet)
- exidl (SDL2, platform support)
- [ImperatorPrime](https://github.com/ImperatorPrime) (JK2)
- [mrwonko](https://github.com/mrwonko)
- [redsaurus](https://github.com/redsaurus)
- [Scooper](https://github.com/xScooper)
- [Sil](https://github.com/TheSil)
- [smcv](https://github.com/smcv) (debian packaging)
- [Tristamus](https://tristamus.com>) (icon)
