# Kontakt 8 Library Removal Tool (K8-LRT)

K8-LRT is a tool for removing libraries for the Bobdule version of Kontakt 8. Currently **Windows only**.

![](docs/screenshot.png)

K8-LRT completely removes Kontakt libraries from the cache on disk. Native Instruments stores cache files in several different locations for some reason and this tool ensures they're all checked and removed.

Locations that need to be searched and deleted:

- `HKEY_LOCAL_MACHINE\SOFTWARE\Native Instruments\<Library Name>`
- `HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Native Instruments` (edge-case, still needs checked)
- `C:\Program Files\Common Files\Native Instruments\Service Center\<Library Name>.xml`
- `~\AppData\Local\Native Instruments\Kontakt 8\LibrariesCache`
- `~\AppData\Local\Native Instruments\Kontakt 8\komplete.db3`

## Install

You can download the latest version of K8-LRT from the [releases](https://github.com/jakerieger/K8-LRT/releases/latest) page.

## Changelog

### 0.1.0 (January 22, 2026)

- Initial release of K8-LRT

## Building

K8-LRT is written in pure C using the Windows API. It can be compiled via `nmake` which is included with the MSVC toolchain.

## License

K8-LRT is licensed under the [Unlicense](LICENSE).