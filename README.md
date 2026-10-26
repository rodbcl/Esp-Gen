# Esp-Gen — Gerador de Pulsos com ESP32

Gerador de pulsos para bancada, feito com ESP32. A frequência e o duty cycle são ajustáveis. A geração termina sozinha depois de um **tempo** ou de uma **quantidade de pulsos** definidos. A configuração é feita por um encoder rotativo e um display LCD 16x2, e a saída de 24 V passa por um estágio push-pull.

Os pulsos saem do periférico **RMT** do ESP32, sem bit-banging nem timers por software. Por isso o sinal tem timing exato, e o número de períodos gerados é exatamente o que foi pedido, mesmo em frequências altas.



## Recursos

- **Frequência** de 1 Hz a 10 kHz
- **Duty cycle** de 1 a 100 %, com resolução de 0,1 µs
- **Quatro modos**
  - **Tempo**: gera durante `mm:ss`, até 60 min
  - **Quantidade**: gera N pulsos, até 10.000
  - **Contínuo**: gera até ser interrompido; frequência e duty podem ser alterados **durante** a geração (nos outros modos ficam fixos enquanto gera)
  - **Automação**: testes de dias ou semanas que imitam uma máquina real (ritmo variável, paradas, ruído, refugos e paradas justificadas), configurados por uma página web no celular. Veja [Modo Automação](#modo-automação)
- **Encoder com aceleração**: quanto mais rápido você gira, maior o passo (x1 → x10 → x100)
- **Configurações salvas na NVS**: os valores voltam iguais depois de desligar
- **Endereço do LCD detectado automaticamente** (PCF8574 em 0x20–0x27 ou PCF8574A em 0x38–0x3F)
- **Botão START/STOP**: toque para iniciar; **segure por 2 s** para interromper a qualquer momento, em qualquer modo (toques curtos durante a geração são ignorados). A tela mostra a contagem regressiva ou o progresso
- **Saída segura no boot**: o pino de pulso é configurado como desligado antes de qualquer outra inicialização

### Esquema elétrico

| Componente | Pino ESP32 |
|---|---|
| LCD 16x2 I2C (PCF8574), via level shifter | SDA = GPIO21, SCL = GPIO22 |
| Encoder KY-040 | CLK = GPIO32, DT = GPIO33, SW = GPIO25 |
| Botão START/STOP (ativo em 0) | GPIO26 |
| Saída de pulsos (performance) | GPIO27 |
| Running (Automação) | GPIO23 |
| Qualidade (Automação) | GPIO19 |
| Stop report (Automação) | GPIO18 |
O esquema elétrico completo não está publicado neste repositório. Para obtê-lo, entre em contato pelo e-mail [rodrigob1706@icloud.com](mailto:rodrigob1706@icloud.com).

### Alimentação

A placa é alimentada pelos 24 V através de um buck **LM2596 ajustado em 5 V**, ligado no pino 5V da DevKitC (nunca junto com a USB). Ponha um **capacitor eletrolítico de 1000 µF** entre 5V e GND, perto do ESP32: sem ele, os picos de corrente do Wi-Fi (calibração do rádio ao entrar na Automação) derrubam a tensão e a placa reinicia por brownout. Ao ligar, se o último reset não foi normal, o LCD mostra o motivo (por exemplo `Reset: brownout`).

### Estágio de saída

```
GPIO27 ──► Q1 (BC547) ──► push-pull Q2/Q3 ──► PULSO (24 V)
```

O Q1 **inverte** o sinal: GPIO27 em nível alto deixa a saída em 0 V. O firmware compensa isso com o `invert_out` do RMT (`PULSE_INVERTED` em [board.h](main/board.h)).

As saídas GPIO23, GPIO19 e GPIO18 usam o mesmo estágio (`OUT_INVERTED`). Todas são configuradas logo no boot, ainda em 0 V.

## Uso

```
 >Modo:     Tempo        RUN 1000Hz 50%
  Freq:    1000Hz        Resta 00:07
```

| Ação | Navegação (`>`) | Edição (`*`) | Rodando | Rodando (Contínuo) |
|---|---|---|---|---|
| Girar encoder | Move entre os itens | Altera o valor | — | Altera o valor marcado (`>`) |
| Clicar encoder | Entra em edição | Confirma e salva | — | Alterna entre Freq e Duty |
| START | Inicia a geração | — | Segurar 2 s: interrompe | Segurar 2 s: interrompe |

Itens do menu: **Modo** (Tempo/Qtde/Continuo/Automacao), **Freq**, **Duty** e **Tempo** ou **Qtde**, conforme o modo escolhido (no modo Contínuo não há esse último item; na Automação só aparece o Modo).

No modo Contínuo a tela mostra o tempo decorrido e os valores ajustáveis:

```
CONT 00:01:23
>1000Hz  50%
```

A nova frequência/duty entra no início do próximo período, sem cortar pulsos. Os valores alterados durante a geração são salvos na NVS ao parar.

## Modo Automação

Escolha **Modo: Automacao** no menu e clique no encoder. O gerador liga um ponto de acesso Wi-Fi e mostra como conectar (a segunda linha alterna entre nome, senha e IP):

```
Conecte-se ao AP
EspGen-3F2A
```

| | |
|---|---|
| Rede | `EspGen-XXXX` (final do MAC) |
| Senha | `espgen123` (`WIFI_AP_PASS` em [wifi_ap.h](main/wifi_ap.h)) |
| Página | `http://192.168.4.1` |

A configuração do teste (faixa de frequência, largura do pulso, paradas, ruído) e o início/fim do teste são feitos pela página. Com um teste rodando, o LCD mostra os pulsos válidos enviados e alterna entre ritmo atual (ou parada), tempo de teste e nome do AP.

- **Saídas extras**, cada uma com a sua chave no perfil (desligada = 0 V o teste todo):
  - **Running** (GPIO23): 24 V com a máquina rodando, 0 V durante as paradas. Nenhum pulso de produção sai com a máquina parada
  - **Qualidade** (GPIO19): uma % dos pulsos de produção vira refugo; o pulso de refugo sai um atraso configurável depois do pulso de produção
  - **Stop report** (GPIO18): paradas justificadas, com intervalo e duração sorteados, independentes das paradas da máquina. Com ele em 24 V o sistema ignora os demais sinais, que continuam normalmente
- **Contabilidade**: o gerador soma separadamente os **pulsos válidos** (o que o contador deve mostrar), os **refugos**, os **ruídos** injetados (que o contador não deve contar) e o que foi enviado com o **stop report** ativo (fica fora do esperado na conferência)
- **Segurar START por 2 s** interrompe o teste, desliga o Wi-Fi e volta ao menu no modo anterior
- **Queda de energia**: o estado é gravado a cada segundo; ao religar, o gerador volta sozinho para a Automação e retoma o teste com a contagem salva. A hora fica "não confirmada" (`≈` na página) até a página ser aberta de novo; aí o histórico é corrigido e a queda ganha a duração real
- **Histórico**: um intervalo por hora de teste (pulsos válidos, refugos, ruídos, paradas, tempo parado, enviados e tempo em stop report, frequência média) e os eventos (início, fim, quedas de energia, conferências), guardados na flash por até ~11 meses de teste
- **CSVs** (aba Histórico): `espgen-intervalos.csv` e `espgen-eventos.csv`, com `;` e vírgula decimal, prontos para o Excel em português, em hora local do celular
- **Conferir contador**: digite o valor do display do contador (e, opcionalmente, os refugos); a diferença para o que foi enviado fora do stop report fica registrada nos eventos

A página é servida pelo próprio ESP ([web/index.html](main/web/index.html), comprimida no build por [web/pack.py](main/web/pack.py)) e não usa nada da internet.

## Compilação

Requer **ESP-IDF v5.4** (o encoder simples do RMT só existe a partir da v5.3).

```bash
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

A tabela de partições é própria ([partitions.csv](partitions.csv)): app de 1,5 MB, `autostate` (estado da Automação, 64 KB) e `history` (histórico dos testes, 512 KB). Ao atualizar de uma versão com outra tabela, grave com `idf.py flash` completo (não só o app). A versão com as saídas extras mudou o formato dos registros: o estado e o histórico anteriores são apagados na primeira inicialização.

## Ajustes

| Define | Arquivo | Padrão | Descrição |
|---|---|---|---|
| Pinos `PIN_*` | [board.h](main/board.h) | — | Ligações do hardware |
| `LCD_COLS` / `LCD_ROWS` | [board.h](main/board.h) | 16 / 2 | Use 20 / 4 para display 20x4 |
| `ENC_COUNTS_PER_DETENT` | [board.h](main/board.h) | 2 | Transições por clique (2 ou 4, depende do encoder) |
| `ENC_REVERSE` | [board.h](main/board.h) | `false` | Inverte o sentido do encoder |
| `PULSE_INVERTED` | [board.h](main/board.h) | `true` | Saída invertida pelo estágio com transistor |
| `FREQ_MAX`, `TIME_MAX_S`, `QTY_MAX` | [settings.h](main/settings.h) | 10 kHz, 3600 s, 10000 | Limites dos valores |
| `STOP_HOLD_MS` | [ui.c](main/ui.c) | 2000 | Tempo segurando START para interromper |

## Estrutura

```
main/
├── app_main.c        # Inicialização: saída segura, NVS, configurações e UI
├── board.h           # Pinos e parâmetros do hardware
├── ui.c / .h         # Máquina de estados do menu, telas e loop da interface
├── run.c / .h        # Execução da geração conforme o modo (Tempo, Qtde, Contínuo)
├── automation.c / .h # Modo Automação: AP, perfil, início/fim do teste, retomada
├── auto_gen.c / .h   # Simulador de máquina: ritmo, paradas, ruído, refugos e stop report (fontes do rmt_out)
├── auto_out.c / .h   # Saídas extras da Automação: running, qualidade e stop report
├── tally.c / .h      # Contabilidade: pulsos válidos, refugos, ruídos, paradas e stop report
├── state_log.c / .h  # Estado do teste gravado em anel na partição autostate
├── history.c / .h    # Histórico (intervalos e eventos), gráfico da última hora e CSVs
├── ringlog.c / .h    # Registro em anel genérico na flash (usado por state_log e history)
├── wallclock.c / .h  # Hora real e fuso (vindos do navegador)
├── wifi_ap.c / .h    # Ponto de acesso Wi-Fi
├── web_server.c / .h # Servidor HTTP: página e API JSON
├── web/index.html    # Página do modo Automação
├── web/pack.py       # Monta a página comprimida embutida no firmware
├── settings.c / .h   # Configurações do usuário, limites e persistência na NVS
├── screen.c / .h     # Buffer da tela: envia ao LCD só as linhas que mudaram
├── encoder.c / .h    # Encoder rotativo via PCNT, com aceleração
├── button.c / .h     # Botões com debounce e pressão longa
├── pulse_gen.c / .h  # Gerador de pulsos da saída principal (fonte de frequência fixa)
├── rmt_out.c / .h    # Canal RMT com encoder em streaming (um por saída)
├── lcd_i2c.c / .h    # Driver HD44780 + PCF8574 (modo 4 bits)
└── util.h            # Funções auxiliares (clamp)
```

### Como funciona o gerador

Cada saída é um canal `rmt_out`, que usa um **RMT simple encoder** para montar os símbolos sob demanda, enquanto a transmissão acontece. Cada trecho (nível e duração) vem de uma **fonte**: a de frequência fixa do `pulse_gen`, usada pelos modos Tempo, Qtde e Contínuo, ou o simulador de máquina (`auto_gen`) da Automação, que tem uma fonte para cada saída. Cada parte da simulação sorteia numa sequência própria a partir da mesma semente, então as fontes de running, qualidade e stop report refazem as mesmas paradas e os mesmos pulsos da produção sem depender uma da outra, e todas seguem a mesma linha do tempo. O caminho da interrupção roda da IRAM (`CONFIG_RMT_ISR_IRAM_SAFE`), para que gravações na flash não atrasem a recarga do RMT. Por isso trens de pulsos com milhões de períodos funcionam sem ocupar RAM. Quando o período não é múltiplo exato do tick de 0,1 µs, o resto é distribuído por um acumulador, de modo que a frequência média fica exata. Cada meio-símbolo dura no máximo 200 µs, e assim um STOP esvazia a memória do RMT em poucos milissegundos.
