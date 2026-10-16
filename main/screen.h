#pragma once

/*
 * Buffer da tela do LCD. A cada quadro: screen_begin(), escreve as linhas e screen_flush(),
 * que completa com espaços e envia ao LCD só as linhas que mudaram.
 * Textos maiores que a largura do display são cortados.
 */

void screen_begin(void);
void screen_printf(int row, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Linha de menu: marcador, rótulo à esquerda e valor alinhado à direita */
void screen_item(int row, char mark, const char *label, const char *value);
void screen_flush(void);
