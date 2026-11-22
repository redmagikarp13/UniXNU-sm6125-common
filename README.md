# UniXNU-sm6125-common

Camada comum do SoC **Qualcomm Snapdragon 665 (`SM6125`)** para o port **UniXNU** (`ipados-17.7.11-21H461`, `arm64`, páginas de **4 KiB**, **ARMv8.0-A** sem PAN/LSE/PAC).

## Componentes Principais

- **`scripts/emmc-probe/Sm6125EmmcCore.{h,c}`**: driver bare-metal de controlador `GCC_SDCC1` + `SDHCI-MSM v5` (DDR52 8-bit, 32-bit ADMA2, leitura `CMD17`/`CMD18`, escrita `CMD24`/`CMD25`, `CMD13`/`DAT0` polling, `CMD6 FLUSH_CACHE`, trava LBA rígida para cauda livre de `userdata`).
- **`scripts/emmc-probe/Sm6125ShngRoot.{h,c}`**: parser e validador de manifesto `SHNGROOT` v1/v2 (janelas `OS`/`SystemOS` e objetos `VerifiedRoot`).
- **`scripts/emmc-probe/Sm6125EmmcBlock.cpp`**: classe `IOBlockStorageDevice` com registro late-metaclass, integração `VerifiedRoot` (RSA-3072), ponte `native_ufs_store_*` para `NativeRamBlock` (`PersistOverlay` `SHNGDATA`) e checagem de integridade GPT.
- **`scripts/build-sm6125-legacy-display.py`**: construtor e adaptador `NativeLegacyDisplay` para `IOMobileFramebufferLegacy` (ABI `ipados-17.7.11-21H461`) + integração `AppleKeyStoreState` sem `__mod_init_func`, com registro late-probe em `/arm-io/gop-display`.
- **`scripts/platform-power/`**: ponte de energia/PMGR (`ApplePMGRNub`, `pmgr=2`) e hooks PSCI.
- **`patches/xnu/`**: série de patches XNU específicos do `SM6125` (`native-smp-secondary`, `sheng-platform-power`, `platform-sleep-hook`, `iokit-fdt-phandle-alias`, `sm6125-modload-lock-order`, `sm6125-emmc-probe-hook`).

## Testes Locais (Host ASan+UBSan e Auditoria Binária)

```bash
# 1. Suítes C do controlador eMMC em host com ASan + UBSan
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I scripts/emmc-probe \
  scripts/emmc-probe/tests/emmc_core_host.c \
  scripts/emmc-probe/Sm6125EmmcCore.c \
  scripts/emmc-probe/Sm6125EmmcReadOnly.c \
  scripts/emmc-probe/Sm6125ShngRoot.c \
  -o /tmp/emmc_core_host && /tmp/emmc_core_host /path/to/gpt_primary.bin

clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I scripts/emmc-probe \
  scripts/emmc-probe/tests/emmc_ro_host.c \
  scripts/emmc-probe/Sm6125EmmcReadOnly.c \
  -o /tmp/emmc_ro_host && /tmp/emmc_ro_host

# 2. Auditoria de ADT, configs, MMIO e instruções ARMv8.0-A
SM6125_KERNEL_MACHO=/path/to/mach.development.bcm2837.unstripped.noctf \
  python3 -m unittest discover -s tests -v
```
