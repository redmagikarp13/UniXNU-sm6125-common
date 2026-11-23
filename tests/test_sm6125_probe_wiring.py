#!/usr/bin/env python3
"""Guardas estaticas de fiacao: touch/HID/botoes/GPU no build do kernel (sem hardware)."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = (ROOT / "scripts/build-sm6125-touch-probe.py").read_text()
PROBE = (ROOT / "scripts/touch-probe/Sm6125TouchProbe.cpp").read_text()
HID = (ROOT / "scripts/touch-probe/Sm6125HidInput.cpp").read_text()


class ProbeWiringTests(unittest.TestCase):
    def test_build_script_compiles_every_probe_source(self):
        for name in ("Sm6125Buttons.c", "Sm6125GeniFifo.c", "Sm6125NvtProbe.c",
                     "Sm6125GpuProbe.c", "Sm6125RpmGlink.c"):
            self.assertIn(name, BUILD, f"{name} fora do build-sm6125-touch-probe.py")

    def test_every_source_in_build_exists(self):
        for name in re.findall(r"(Sm6125\w+\.(?:cpp|c))\b", BUILD):
            if name == "Sm6125TouchFw.c":  # gerado a partir do firmware proprietario (nunca versionado)
                continue
            hits = list((ROOT / "scripts").rglob(name))
            self.assertTrue(hits, f"{name} referenciado no build mas ausente")

    def test_hid_descriptor_order_touch_first(self):
        # Regressao Marco 6C: touch primeiro, teclado/consumer depois, senao o
        # backboardd ignora os reports (pu=0x0D/0x04 exige touch no inicio).
        i = HID.index("memcpy(combined, kTouchDescriptor")
        j = HID.index("memcpy(combined + sizeof(kTouchDescriptor), kKeyboardConsumerDescriptor")
        self.assertLess(i, j)

    def test_gpu_probe_is_opt_in_and_staged(self):
        self.assertTrue(re.search(r'PE_parse_boot_argn\(\s*"sm6125-gpu"', PROBE))
        self.assertIn("sm6125_gpu_probe_stage", PROBE)
        # desligado por padrao: sem boot-arg (ou 0) retorna antes de tocar o hardware
        self.assertTrue(re.search(r"gpu_req == 0u\)\s*\{\s*return;", PROBE))

    def test_gpu_bisect_opt_arg_present(self):
        self.assertIn("sm6125-gpu-opt", PROBE)

    def test_gpu_probe_never_pokes_before_rpm_votes(self):
        stage_call = re.search(r"sm6125_gpu_probe_stage\(\s*&gbus", PROBE)
        vote = re.search(r"SM6125_RPM_TYPE_RWCX", PROBE)
        self.assertTrue(stage_call and vote)
        self.assertLess(vote.start(), stage_call.start())

    def test_gpu_gcc_base_matches_sm6125_gcc_base(self):
        touch_board = (ROOT / "scripts/touch-probe/Sm6125TouchBoard.h").read_text()
        gpu_board = (ROOT / "scripts/gpu-probe/Sm6125GpuBoard.h").read_text()
        m_gcc = re.search(r"#define\s+SM6125_GCC_BASE\s+(0x[0-9A-Fa-f]+)", touch_board)
        m_vote = re.search(r"#define\s+SM6125_GCC_GPU_VOTE_BASE\s+(0x[0-9A-Fa-f]+)", gpu_board)
        self.assertTrue(m_gcc and m_vote)
        self.assertEqual(int(m_vote.group(1), 16), int(m_gcc.group(1), 16) + 0x79000)


if __name__ == "__main__":
    unittest.main()
