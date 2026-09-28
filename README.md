# Windows VHF Ambient Light Sensor Test Driver

This is a **test-only** Windows KMDF/VHF virtual HID Ambient Light Sensor. It generates synthetic lux readings to exercise Windows Sensor API enumeration and report handling. It is not required for the physical RP2040 sensor and is not a production or signed distribution driver. Physical firmware uses the Windows inbox HID sensor driver.

## Contents

* `tools/vhf-als/VhfAlsSim.c` — KMDF/VHF simulator source.
* `tools/vhf-als/VhfAlsSim.inf` — test device installation metadata.
* `tools/vhf-als/AlsReportDescriptor.h` — descriptor snapshot matching the RP2040 firmware HID descriptor.
* `tools/vhf-als/VhfAlsSim.vcxproj` — Visual Studio driver project.

## Build

Use Visual Studio with the Windows Driver Kit (WDK), KMDF, and the VHF development library. Open the project and build the x64 configuration. The project currently names Windows SDK/WDK `10.0.28000.0` and KMDF 1.35; adjust those project properties to match an installed compatible WDK if needed.

Installation requires administrator privileges, a test-signing configuration, and a locally signed catalog. Follow Microsoft's current driver test-signing guidance on a disposable test system. Do not install this simulator on a machine where synthetic readings could be mistaken for a physical sensor. Disable or remove the virtual device after testing.

This repository intentionally omits machine-specific certificates, signing scripts, diagnostics, binaries, and catalog files. It does not provide a production-signing certificate or production installation package.

## Report descriptor maintenance

The header is generated from the canonical `ALS_REPORT_DESCRIPTOR` byte array in the firmware repository's `src/main.rs`. Keep both byte sequences aligned when the HID descriptor changes. The repository source in this project family has a validator that checks this correspondence.

## Verification scope

The simulator has previously enumerated as a Windows Sensor and produced synthetic 50/500-lux readings through the native API after report descriptor and enum fixes. This demonstrates the Windows API test path only; it does not establish physical sensor accuracy or certify this as a distributable driver.
