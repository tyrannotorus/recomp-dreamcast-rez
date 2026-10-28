# Rez Recompiled
Rez (Sega Dreamcast, 2001) is one of my favourite games.
This is a recomp of Rez for Linux and the TrimUI Brick. It has been statically recompiled to C++.
Unlike many recomps, this one does not rely on hardware processor simulation. A large amount of
time has been spent on performance, especially targeting the TrimUI Brick.

<img width="1024" height="768" alt="rez-005" src="https://github.com/user-attachments/assets/53d0e6d9-c97f-4733-afb9-8ba0cb8bdd46" />

<img width="1024" height="768" alt="rez-010" src="https://github.com/user-attachments/assets/d5418f5a-0142-4789-b04d-3334e38d03d8" />

<img width="1024" height="768" alt="rez-012" src="https://github.com/user-attachments/assets/588c3baa-e580-4023-8499-8f2649b6c4c1" />

<img width="1024" height="768" alt="rez-013" src="https://github.com/user-attachments/assets/2067e11c-ba29-4d92-b376-ee98f64e20c7" />

<img width="1024" height="768" alt="rez-034" src="https://github.com/user-attachments/assets/281baad0-005b-49e1-899a-efa077710aad" />

<img width="1024" height="768" alt="rez-035" src="https://github.com/user-attachments/assets/e255606d-0a20-4ed0-ac20-830b8a9aeaba" />

<img width="1024" height="768" alt="rez-040" src="https://github.com/user-attachments/assets/8038c7d9-c633-4f3a-a595-bf27a1aa5742" />

## Current Features
- Plays from the Japanese and European releases (V1.003 .chd).
- Game has been retooled to run at 60hz with optional widescreen.
- Linux PC: Vulkan renderer, SDL3 window, sound and controllers.
- TrimUI Brick: full speed (60 fps) at 640x480. 1024x768 is supported.

## Known Issues
- None known

## Dev Disclaimer
While I am an industry veteran, note this project has been human-directed but 100% slop-coded. I've
spent considerable time directing the code, all the while removing AI cruft, needless verbosity, and
architectural slop, and deleting dead, redundant, pointless code to prevent it from clogging up the
project, but I still cannot guarantee "thoughtfully-engineered code" at the ground level like I do my
own work. You are welcome to fork, but beware, here be dragons.

## Install on the TrimUI Brick (NextUI)
1. Download `Rez.pak.zip` from [Releases](https://github.com/tyrannotorus/recomp-dreamcast-rez/releases).
2. Unzip it to `Tools/tg5040/Rez.pak/` on the SD card.
3. Copy your Rez `.chd` (Japanese or European) to `Tools/tg5040/Rez.pak/disc/`.
4. Start Tools > Rez. MENU quits.

## Building for Linux and the Brick
See [BUILDING.md](BUILDING.md).

## Credits
Rez Recompiled is heavily indebted to
- [**dream-recomp**](https://github.com/phobos665/dream-recomp)
- [**dreamcomp**](https://github.com/cherryduck/dreamcomp)
- [**flycast**](https://github.com/flyinghead/flycast)

## Licence

GPL-2.0 ([LICENSE](LICENSE)).
