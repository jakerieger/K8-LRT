# Kontakt 8 Library Removal Tool (K8Tool)

K8Tool is a tool for removing libraries from the Bobdule version of Kontakt 8 (**Windows only**).

![](docs/screenshot.png)

K8Tool completely removes Kontakt libraries from the cache on disk. Native Instruments stores cache files in several
different locations for some reason and this tool ensures they're all checked and removed.

Locations that need to be searched and deleted:

- `HKEY_LOCAL_MACHINE\SOFTWARE\Native Instruments\<Library Name>`
- `HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Native Instruments\<Library Name>` (edge-case, still needs checked)
- `C:\Program Files\Common Files\Native Instruments\Service Center\<Library Name>.xml`
- `~\AppData\Local\Native Instruments\Kontakt 8\LibrariesCache\<filename>`
- `~\AppData\Local\Native Instruments\Kontakt 8\komplete.db3`
- `C:\Users\Public\Documents\Native Instruments\Native Access\ras3\<filename>.jwt`
- `C:\Program Files\Common Files\Native Instruments\Kontakt 8\PAResources` [?]

## What It Does

This process of steps is executed by the program to remove libraries. In theory, you could do all
of this manually. K8Tool just makes it a lot easier.

1. Locate library entries in the registry. These are located under two locations:
    - `HKEY_LOCAL_MACHINE\SOFTWARE\Native Instruments`              (**Primary**)
    - `HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Native Instruments`  (**Secondary**, *rare*)
2. Library entries have a `ContentDir` value that stores the location of the actual library on
   disk. We store this and the library name retrieved from the registry key to a list.
3. When a library is selected for removal, we take the following actions:
    1. Find the corresponding <LibraryName>.xml file located in:
        - `C:\Program Files\Common Files\Native Instruments\Service Center`
    2. If it doesn't exist, check the `NativeAccess.xml` file in the same path for an entry.
    3. Save the `SNPID` value from the XML file and delete it (DO NOT REMOVE NativeAccess.xml)
    4. Find the corresponding .cache file located in:
        - `~\AppData\Local\Native Instruments\Kontakt 8\LibrariesCache`
        - The filename has the format "K{SNPID}...".cache
    5. Delete the .cache file.
    6. Delete and create a backup of `~\AppData\Local\Native Instruments\Kontakt 8\komplete.db3`.
       Kontakt will rebuild this next time it's launched.
    7. Look for the associated `.jwt` file located in:
        - `C:\Users\Public\Documents\Native Instruments\Native Access\ras3`
    8. Delete the .jwt file.
    9. Delete the library content directory (if the user selected to do so).
    10. Delete the registry key (and create a backup if requested).
4. Relocating a library simply involves moving the content directory to the new location
   and updating the `ContentDir` registry value

## Quickstart

The latest version of K8Tool is v2.0.0 - you
can [download it here](https://github.com/jakerieger/K8Tool/releases/latest).

Check out the [Quickstart](docs/QUICKSTART.md) guide for info on how to use K8Tool.

## Changelog

### 2.0.0 (January 31, 2026)

The 2.0 release of K8Tool brings a lot of bug fixes and reworks a large portion of the code base to improve both
reliability and maintainability.

#### Added

- String pools for more robust memory management
- The ability to relocate libraries
- Full NTFS-length path support

#### Fixed

- Crashing if an incorrect value for 'ContentDir' is found in the registry. Now it should simply ignore the content
  directory when removing the library (and disallow relocating since it doesn't have a location on disk).

#### Removed

- Support for Windows 7

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

The official 1.0 release of K8Tool.

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

- Initial release of K8Tool

## Building

K8Tool is written in pure C using the Windows API. The entire program is just under 2000 lines of code and the
executable is only about 300 Kb.

K8Tool can be compiled via `nmake`, which is included with the MSVC toolchain. This requires the C++ Visual Studio
toolsuite to be installed. If it is, you can simply run the build script from PowerShell to build K8Tool:

```powershell
.\build.ps1 -Config debug # or release
```

## License

K8Tool is licensed under the [ISC license](LICENSE).