# blitzunlink

blitzunlink is a tool for converting BlitzBasic, BlitzPlus, BlitzMax, Blitz3D game bytecode into easier to reverse and mod DLLs

## How to use

0. Install mingw-w64 32-bit, this will be needed later.

1. Compile the `loader` branch of [my fork of Blitz3D](https://github.com/namazso/blitz3d_msvc2017)

2. Compile this project

3. Extract the `1111` ID RCDATA resource from the target game (you can use 7-Zip for this):

![image](https://user-images.githubusercontent.com/8676443/149443161-1e3e6e29-df5d-4a84-a235-55d992eae47b.png)

4. Convert the game code blob into a COFF object with this tool:

```
bbunlink 1111 bbgame.obj
```

5. Link the COFF object into a DLL using `gcc`. `runtime.dll` is the file from the outputs of the Blitz3D compile:

```
gcc -s -shared -e_DllEntry -nodefaultlibs -nostdlib -Wl,--enable-runtime-pseudo-reloc-v1 -Wl,--unique=.rdata bbgame.obj runtime.dll -o bbgame.dll
```

6. Copy `runtime_ldr.exe`, `runtime.dll`, `bbgame.dll`, `fmod.dll` into the game folder.

7. Game should be functional when starting `runtime_ldr.exe`

## License Statement

	blitzunlink - A Blitz blob to COFF converter.
	Copyright (C) 2022  namazso
	
	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
	
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
	
	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <https://www.gnu.org/licenses/>.
