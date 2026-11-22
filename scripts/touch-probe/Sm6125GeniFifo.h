// SPDX-License-Identifier: MIT
// Transferencia SPI GENI em modo FIFO PIO (> 60 bytes, ex.: host-download FW 4 KiB e pacotes de toque).
#ifndef SM6125_GENI_FIFO_H
#define SM6125_GENI_FIFO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "GeniSpi.h"

/* drivers/soc/qcom/geni-se.h: TX_FIFO_WC = GENMASK(27, 0) em SE_GENI_TX_FIFO_STATUS. */
#define SM6125_GENI_TX_WC_MASK 0x0FFFFFFFu

/* Executa transferencia SPI em modo FIFO (TX_ONLY, RX_ONLY ou TX_RX) de qualquer tamanho com seguranca:
 * - checa espaco livre em SE_GENI_TX_FIFO_STATUS antes de escrever em SE_GENI_TX_FIFOn;
 * - drena RX e reabastece TX ANTES de escrever SE_GENI_M_IRQ_CLEAR (watermarks latched-level);
 * - captura snap_irq/status/txfs/rxfs antes de limpar IRQs ou recuperar o motor em erro.
 */
int sm6125_geni_fifo_transfer(
    struct geni_spi *s,
    const uint8_t *tx,
    uint8_t *rx,
    size_t len,
    int keep_cs);

#ifdef __cplusplus
}
#endif

#endif
