# Esp-Gen — Gerador de Pulsos com ESP32

Gerador de pulsos para bancada, feito com ESP32. A frequência e o duty cycle são ajustáveis. A geração termina sozinha depois de um **tempo** ou de uma **quantidade de pulsos** definidos. A configuração é feita por um encoder rotativo e um display LCD 16x2, e a saída de 24 V passa por um estágio push-pull.

Os pulsos saem do periférico **RMT** do ESP32, sem bit-banging nem timers por software. Por isso o sinal tem timing exato, e o número de períodos gerados é exatamente o que foi pedido, mesmo em frequências altas.

## Recursos

- **Frequência** de 1 Hz a 10 kHz
- **Duty cycle** de 1 a 100 %, com resolução de 0,1 µs
- **Dois modos de parada**
  - **Tempo**: gera durante `mm:ss`, até 60 min
  - **Quantidade**: gera N pulsos, até 10.000
- **Encoder com aceleração**: quanto mais rápido você gira, maior o passo (x1 → x10 → x100)
- **Configurações salvas na NVS**: os valores voltam iguais depois de desligar
- **Endereço do LCD detectado automaticamente** (PCF8574 em 0x20–0x27 ou PCF8574A em 0x38–0x3F)
- **Botão START/STOP** para iniciar ou interromper a qualquer momento, com contagem regressiva ou progresso na tela
- **Saída segura no boot**: o pino de pulso é configurado como desligado antes de qualquer outra inicialização

## Hardware

| Componente | Pino ESP32 |
|---|---|
| LCD 16x2 I2C (PCF8574), via level shifter | SDA = GPIO21, SCL = GPIO22 |
| Encoder KY-040 | CLK = GPIO32, DT = GPIO33, SW = GPIO25 |
| Botão START/STOP (ativo em 0) | GPIO26 |
| Saída de pulsos | GPIO27 |

### Estágio de saída

```
GPIO27 ──► Q1 (BC547) ──► push-pull Q2/Q3 ──► PULSO (24 V)
```

O Q1 **inverte** o sinal: GPIO27 em nível alto deixa a saída em 0 V. O firmware compensa isso com o `invert_out` do RMT (`PULSE_INVERTED` em [app_main.h](main/app_main.h)).

## Uso

```
 >Modo:     Tempo        RUN 1000Hz 50%
  Freq:    1000Hz        Resta 00:07
```

| Ação | Navegação (`>`) | Edição (`*`) | Rodando |
|---|---|---|---|
| Girar encoder | Move entre os itens | Altera o valor | — |
| Clicar encoder | Entra em edição | Confirma e salva | — |
| START | Inicia a geração | — | Interrompe |

Itens do menu: **Modo** (Tempo/Qtde), **Freq**, **Duty** e **Tempo** ou **Qtde**, conforme o modo escolhido.

## Compilação

Requer **ESP-IDF v5.4** (o encoder simples do RMT só existe a partir da v5.3).

```bash
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Ajustes

Os principais parâmetros ficam em [main/app_main.h](main/app_main.h):

| Define | Padrão | Descrição |
|---|---|---|
| `LCD_COLS` / `LCD_ROWS` | 16 / 2 | Use 20 / 4 para display 20x4 |
| `ENC_COUNTS_PER_DETENT` | 2 | Transições por clique (2 ou 4, depende do encoder) |
| `ENC_REVERSE` | `false` | Inverte o sentido do encoder |
| `PULSE_INVERTED` | `true` | Saída invertida pelo estágio com transistor |
| `FREQ_MAX`, `TIME_MAX_S`, `QTY_MAX` | 10 kHz, 3600 s, 10000 | Limites dos valores |

## Estrutura

```
main/
├── app_main.c / .h   # UI, encoder (PCNT), botões, NVS e loop principal
├── pulse_gen.c / .h  # Gerador de pulsos via RMT com encoder em streaming
└── lcd_i2c.c / .h    # Driver HD44780 + PCF8574 (modo 4 bits)
```

### Como funciona o gerador

O `pulse_gen` usa um **RMT simple encoder** que monta os símbolos sob demanda, enquanto a transmissão acontece. Por isso trens de pulsos com milhões de períodos funcionam sem ocupar RAM. Quando o período não é múltiplo exato do tick de 0,1 µs, o resto é distribuído por um acumulador, de modo que a frequência média fica exata. Cada meio-símbolo dura no máximo 200 µs, e assim um STOP esvazia a memória do RMT em poucos milissegundos.
