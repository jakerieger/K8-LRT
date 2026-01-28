# Kontakt 8 Library Removal Tool (K8-LRT)

K8-LRT is a tool for removing libraries from the Bobdule version of Kontakt 8 (**Windows only**).

![](docs/screenshot.png)

K8-LRT completely removes Kontakt libraries from the cache on disk. Native Instruments stores cache files in several different locations for some reason and this tool ensures they're all checked and removed.

Locations that need to be searched and deleted:

- `HKEY_LOCAL_MACHINE\SOFTWARE\Native Instruments\<Library Name>`
- `HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Native Instruments\<Library Name>` (edge-case, still needs checked)
- `C:\Program Files\Common Files\Native Instruments\Service Center\<Library Name>.xml`
- `~\AppData\Local\Native Instruments\Kontakt 8\LibrariesCache\<filename>`
- `~\AppData\Local\Native Instruments\Kontakt 8\komplete.db3`
- `C:\Users\Public\Documents\Native Instruments\Native Access\ras3\<filename>.jwt`
- `C:\Program Files\Common Files\Native Instruments\Kontakt 8\PAResources` [?]

## Quickstart

The latest version of K8-LRT is v1.1.0 - you can [download it here](https://github.com/jakerieger/K8-LRT/releases/latest).

Check out the [Quickstart](docs/QUICKSTART.md) guide for info on how to use K8-LRT.

## Changelog

### 1.1.0 (January 26, 2026)

#### Added
- Removes additional files associated with the library cache (thanks to Bobdule for the info)
- Removal dialogs for confirming options prior to removing one or multiple libraries

#### Changed
- Condensed "File" and "Help" menus into a single menu
- Label text changes

#### Fixed
- Bug with checkbox values not persisting

### 1.0.0 (January 25, 2026)

The official 1.0 release of K8-LRT.

#### Added
- Update checking
- Log viewer
- Manually reload libraries

#### Changed
- Updated UI look to modern Windows

#### Fixed
- Memory leak constructing path strings
- Inconsistent naming
- Small bug fixes with file I/O

### 0.3.1 (January 23, 2026)

- Improved memory management model
- Added backups for cache files

### 0.3.0 (January 23, 2026)

- Sweeping code changes and refactoring
- Bug fixes and improvements
- Logging for debugging issues if any arise
- Library entry filtering to remove non-library entries
- More robust error handling

### 0.2.0 (January 23, 2026)

- Tons of bug fixes and improvements
- Added a `Remove All` button in place of the quit button
- Filter registry entries to remove non-library entries
- General code improvements

### 0.1.0 (January 22, 2026)

- Initial release of K8-LRT

## Building

K8-LRT is written in pure C using the Windows API. The entire program is just under 2000 lines of code and the executable is only about 300 Kb. 

K8-LRT can be compiled via `nmake`, which is included with the MSVC toolchain. This requires the C++ Visual Studio toolsuite to be installed. If it is, you can simply run the build script from PowerShell to build K8-LRT:

```powershell
.\build.ps1 -Config debug # or release
```

## License

K8-LRT is licensed under the [Unlicense](LICENSE).