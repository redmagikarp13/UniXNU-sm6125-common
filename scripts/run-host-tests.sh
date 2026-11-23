#!/usr/bin/env bash
# Harness unico de testes de host (ASan+UBSan) + auditorias Python do UniXNU-sm6125-common.
# Uso: scripts/run-host-tests.sh [caminho/para/gpt_primary.bin]
# Variaveis: SM6125_KERNEL_MACHO (opcional; habilita auditoria binaria ARMv8.0-A).
set -euo pipefail
cd "$(dirname "$0")/.."
CC="${CC:-clang}"
FLAGS=(-std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined)
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
GENI_DIR="../UniXNU/drivers/QcomGeniSpi"

run() { echo "== $1"; shift; "$@"; }

run "eMMC core" bash -c "'$CC' ${FLAGS[*]} -I scripts/emmc-probe scripts/emmc-probe/tests/emmc_core_host.c \
  scripts/emmc-probe/Sm6125EmmcCore.c scripts/emmc-probe/Sm6125EmmcReadOnly.c scripts/emmc-probe/Sm6125ShngRoot.c \
  -o '$OUT/emmc_core' && '$OUT/emmc_core' ${1:-}"
run "eMMC read-only" bash -c "'$CC' ${FLAGS[*]} -I scripts/emmc-probe scripts/emmc-probe/tests/emmc_ro_host.c \
  scripts/emmc-probe/Sm6125EmmcReadOnly.c -o '$OUT/emmc_ro' && '$OUT/emmc_ro'"
run "touch + botoes (GENI/NT36672A/SPMI)" bash -c "'$CC' ${FLAGS[*]} -I scripts/touch-probe -I '$GENI_DIR' \
  scripts/touch-probe/tests/Sm6125TouchTests.c scripts/touch-probe/Sm6125NvtProbe.c scripts/touch-probe/Sm6125GeniFifo.c scripts/touch-probe/Sm6125Buttons.c '$GENI_DIR/GeniSpi.c' \
  -o '$OUT/touch' && '$OUT/touch'"
run "GPU (GCC/GPUCC/RBBM/SMMU/GLINK-RPM/estagios)" bash -c "'$CC' ${FLAGS[*]} -I scripts/gpu-probe \
  scripts/gpu-probe/tests/Sm6125GpuTests.c scripts/gpu-probe/Sm6125GpuProbe.c scripts/gpu-probe/Sm6125RpmGlink.c \
  -o '$OUT/gpu' && '$OUT/gpu'"
run "auditorias Python" python3 -m unittest discover -s tests -v
echo "== OK: todas as suites de host passaram"
