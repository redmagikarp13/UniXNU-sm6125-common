# UniXNU-sm6125-common

Camada comum do SoC **Qualcomm Snapdragon 665 (`SM6125`)** para o port **UniXNU** (`ipados-17.7.11-21H461`, `arm64`, páginas de **4 KiB**, **ARMv8.0-A** sem PAN/LSE/PAC).

## Componentes Principais

- **`scripts/emmc-probe/Sm6125EmmcCore.{h,c}`**: driver bare-metal de controlador `GCC_SDCC1` + `SDHCI-MSM v5` (DDR52 8-bit, 32-bit ADMA2, leitura `CMD17`/`CMD18`, escrita `CMD24`/`CMD25`, `CMD13`/`DAT0` polling, `CMD6 FLUSH_CACHE`, trava LBA rígida para cauda livre de `userdata`).
- **`scripts/emmc-probe/Sm6125ShngRoot.{h,c}`**: parser e validador de manifesto `SHNGROOT` v1/v2 (janelas `OS`/`SystemOS` e objetos `VerifiedRoot`).
- **`scripts/emmc-probe/Sm6125EmmcBlock.cpp`**: classe `IOBlockStorageDevice` com registro late-metaclass, integração `VerifiedRoot` (RSA-3072), ponte `native_ufs_store_*` para `NativeRamBlock` (`PersistOverlay` `SHNGDATA`) e checagem de integridade GPT.
- **`scripts/build-sm6125-legacy-display.py`**: construtor e adaptador `NativeLegacyDisplay` para `IOMobileFramebufferLegacy` (ABI `ipados-17.7.11-21H461`) + integração `AppleKeyStoreState` sem `__mod_init_func`, com registro late-probe em `/arm-io/gop-display`.
- **`scripts/touch-probe/` e `scripts/build-sm6125-touch-probe.py`**: sonda SPI `QUPv3 Wrap0 SE2` (`0x04a88000`) + `TLMM WEST` (`GPIO 6..9`, `87`, `88`) + protocolo Novatek `NT36672A` (`sm6125-touch=1..3` diagnóstico; **`=5` = driver HID ao vivo, padrão em `UniXNU-ginkgo/board.toml`**). Validada em silício real (`13bafbaa` / `ginkgo`): modo FIFO PIO (`ifd=0`), DFS (`19..120 MHz`), loopback `16/16`, SPI Mode 0 active-LOW, Chip ID `0a0000726603` (`NT36672A`) e toque funcional no SpringBoard.
  - **`Sm6125HidInput.cpp`**: `IOHIDDevice` único com `kTouchDescriptor` **primeiro** e `kKeyboardConsumerDescriptor` depois (ordem inversa faz o `backboardd` ignorar os reports — regressão Marco 6C coberta por `tests/test_sm6125_probe_wiring.py`).
  - **`Sm6125Buttons.{h,c}`**: Power / Vol− (PON `0x0810`) e Vol+ (PM6125 GPIO5 `0xC408`, active-LOW) via **SPMI PMIC Arbiter v5** (`0x01C40000` / `0x03E00000`) → Report 2 do HID.
- **`scripts/gpu-probe/` (Marco 8A — validado em silício `13bafbaa`, `sm6125-gpu=N` (1 = só RPM; 2..4 = estágios 1..3; >=5 sonda completa), desligada por padrão)**: Adreno 610 (`kgsl-3d0@0x05900000`, sem GMU).
  - **`Sm6125RpmGlink.{h,c}`**: cliente mínimo GLINK-RPM (`rpm_msg_ram 0x045F0000` + doorbell `APCS 0x0F111008`), `VERSION`/`OPEN("rpm_requests")` e votos `rwcx`/`rwmx` (validado: `rc=0/0/0/0`, `msg#` sem `err`).
  - **`Sm6125GpuProbe.{h,c}`**: voto GCC (`0x01479004`), GPUCC (`0x05990000`: `CX_GDSCR=0xf8222000`, `GX_GDSCR=0xf8222000`, `GX_GFX3D_CBCR=0x00004221`), Adreno 610 RBBM (`RBBM_STATUS=0x1`, `RBBM_SP_HYST_CNT` R/W `0x12345678 -> 0x678`) e KGSL SMMU v500 (`IDR0=0x4c017e04`, `IDR1=0x20000005` / 5 CBs, `IDR7=0x00000024`).
- **`scripts/platform-power/`**: ponte de energia/PMGR (`ApplePMGRNub`, `pmgr=2`) e hooks PSCI.
- **`patches/xnu/`**: série de patches XNU específicos do `SM6125` (`native-smp-secondary`, `sheng-platform-power`, `platform-sleep-hook`, `iokit-fdt-phandle-alias`, `sm6125-modload-lock-order`, `sm6125-emmc-probe-hook`).

## Testes Locais (Host ASan+UBSan e Auditoria Binária)

Harness único (compila e roda todas as suítes C com `-Werror -fsanitize=address,undefined` e depois as auditorias Python):

```bash
scripts/run-host-tests.sh [/path/to/gpt_primary.bin]   # gpt é opcional (eMMC core)
# Auditoria binária ARMv8.0-A (opcional):
SM6125_KERNEL_MACHO=/path/to/mach.development.bcm2837.unstripped.noctf scripts/run-host-tests.sh
```

| Suíte | Cobre |
|---|---|
| `emmc-probe/tests/emmc_core_host.c`, `emmc_ro_host.c` | controlador SDHCI, ADMA2, trava LBA, `SHNGROOT` |
| `touch-probe/tests/Sm6125TouchTests.c` | board, GCC/TLMM, GENI FIFO, NT36672A, SPMI buttons |
| `gpu-probe/tests/Sm6125GpuTests.c` | GCC/GPUCC/GDSC/RBBM/SMMU simulados, RPM simulado (handshake, wrap de FIFO, timeout `-10`/`-20`, rejeição `err` `-22`), estágios 1..3 |
| `tests/*.py` | ADT, config do kernel, fiação das sondas/HID (`test_sm6125_probe_wiring.py`) |

**Política TDD:** todo código novo em `scripts/*-probe/*.c` nasce com teste de host vermelho primeiro; a lógica específica de kernel (`*.cpp`) deve ser fina e delegar ao `.c` testável. Validação em silício é registrada em `HANDOFF_STATUS.md` (leitura bruta + imagem + SHA-256).
