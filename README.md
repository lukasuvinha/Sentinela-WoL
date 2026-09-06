# melchior-wol

Firmware para ESP32 que envia um pacote Wake-on-LAN e liga o **melchior**
(notebook Acer Aspire E1-571 rodando Ubuntu Server, homelab pessoal).

O dispositivo e uma sentinela: verifica de tempos em tempos se o
melchior esta no ar e, se nao estiver, liga ele.

```
        ┌──────────────────────────────────────────┐
        │                                          │
        v                                          │
   [ ping 192.168.1.100 ]                         │
        │                                          │
   respondeu? ──── sim ──> ONLINE ── espera 5min ──┘
        │
        nao
        │
        v
   [ envia magic packet para AA:BB:CC:DD:EE:FF ]
        │
   espera 90s (tempo de boot do Ubuntu)
        │
        └──> volta a pingar. Se ainda nao subiu, repete
             indefinidamente ate ligar. A partir da 4a
             tentativa o intervalo passa para 5 min.
```

O projeto roda exclusivamente em ESP32 fisico. Nao ha simulador no
fluxo: build e teste acontecem na propria placa, via USB e serial
monitor.

### Por que ping por IP e nao pelo MAC

Nao da para monitorar so pelo MAC. ICMP (ping) e camada 3 e exige um
endereco IP — nao existe "pingar um MAC". O equivalente em camada 2
seria ARP, mas ARP e uma consulta indexada por IP ("quem tem
192.168.1.100?"), entao o IP continua sendo necessario.

Ha um agravante: NICs armadas para Wake-on-LAN ficam parcialmente
energizadas com a maquina desligada, e algumas respondem ARP nesse
estado. ARP reportaria "ligado" para um servidor desligado — o oposto
do que se quer detectar. ICMP exige a pilha de rede do SO no ar, que e
exatamente a condicao a verificar.

O IP do melchior e fixo por netplan, entao nao muda sozinho.

---

## Estrutura de arquivos

Versionado (vai para o GitHub):

```
melchior-wol/
├── README.md                 # este arquivo
├── .gitignore                # protege credenciais e artefatos de build
├── platformio.ini            # env esp32dev, platform fixado em 7.1.1
├── src/
│   ├── main.cpp              # firmware
│   └── secrets.example.h     # template publico, so placeholders
└── .vscode/
    └── extensions.json       # recomenda a extensao PlatformIO
```

Local, nunca versionado (ver `.gitignore`):

```
├── src/secrets.h             # credenciais reais
├── .pio/                     # build cache; o firmware guarda a senha em texto claro
├── .vscode/c_cpp_properties.json, launch.json
└── .claude/settings.local.json
```

O que cada arquivo do `src/` faz:

| Arquivo | Papel |
|---|---|
| `main.cpp` | Firmware. Inclui `secrets.h` e usa `SECRET_WIFI_SSID`/`SECRET_WIFI_PASSWORD`. MAC do melchior fixo no vetor `TARGET_MAC`. |
| `secrets.example.h` | Template publicavel. Contem os placeholders `PREENCHER_*`, nunca valores reais. Serve de referencia para quem clonar o repo. |
| `secrets.h` | Credenciais reais da rede. Esta no `.gitignore`. Se nao existir, criar com `cp src/secrets.example.h src/secrets.h`. |

---

## Configuracao

### Wi-Fi

A rede precisa ser **2.4 GHz** — o ESP32 nao fala 5 GHz. Alguns roteadores
domesticos anunciam o mesmo SSID nas duas bandas; nesses casos vale
confirmar que a banda 2.4 esta ativa.

Editar `src/secrets.h` e trocar os dois placeholders:

```c
#define SECRET_WIFI_SSID     "PREENCHER_SSID_AQUI"
#define SECRET_WIFI_PASSWORD "PREENCHER_SENHA_AQUI"
```

### Alvo

Ambos ja fixados no `main.cpp`. O MAC serve para acordar, o IP para
verificar se acordou:

```c
byte TARGET_MAC[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
IPAddress MELCHIOR_IP(192, 168, 1, 100);
```

O MAC e o da interface cabeada do melchior — o MAC do proprio ESP32 nao
entra na equacao. Se o IP do melchior mudar, atualizar `MELCHIOR_IP`,
senao a sentinela vai achar que ele esta sempre desligado.

### Toolchain

O `platformio.ini` fixa `platform = espressif32@7.1.1` em vez de deixar
`espressif32` solto. Sem isso, quem clonar o repo daqui a um ano pega
outra versao do toolchain e pode nao compilar. Para atualizar de
proposito, subir o numero e recompilar.

Nao ha `lib_deps`: o ping usa a API nativa do ESP-IDF (`ping_sock.h`),
que ja vem no `liblwip.a` do core. O projeto nao depende de nenhuma
biblioteca externa.

### Tempos

Constantes no topo do `main.cpp`:

| Constante | Valor | Papel |
|---|---|---|
| `INTERVALO_MONITORAMENTO_MS` | 5 min | Espera entre checagens com o melchior no ar. |
| `ESPERA_POS_WOL_MS` | 90 s | Tempo dado ao Ubuntu para subir a rede depois do WoL. |
| `INTERVALO_RETENTATIVA_MS` | 5 min | Espera entre WoL depois das tentativas rapidas. |
| `TENTATIVAS_RAPIDAS` | 3 | Quantos WoL a 90 s antes de espacar para 5 min. |
| `PINGS_POR_CHECAGEM` | 3 | Basta 1 resposta para considerar online. |

O escalonamento existe para nao martelar a rede quando o melchior esta
fisicamente fora da tomada. Ele nunca desiste — so passa a insistir mais
devagar.

Se o melchior demorar mais de 90 s para responder ao ping depois de
ligar, o firmware manda um WoL a mais sem necessidade. Isso e inofensivo
(magic packet em maquina ligada nao faz nada), mas se incomodar, aumentar
`ESPERA_POS_WOL_MS`.

### Broadcast

Fixado em `255.255.255.255` (limited broadcast). Nao depende da faixa de
IP da rede local — trocar de roteador ou provedor nao exige mexer aqui.

---

## Segurança

O firmware embute o SSID e a senha **em texto claro** dentro do binario.
Isso nao e um defeito deste projeto — e como qualquer sketch Arduino
funciona. Da para conferir:

```bash
strings .pio/build/esp32dev/firmware.bin | grep -i <seu-ssid>
```

Consequencia pratica: **nunca** publicar `.pio/`, nem anexar um
`firmware.bin` compilado em issue, gist ou release. O `.gitignore`
cobre `.pio/`, `*.bin`, `*.elf` e `*.map` justamente por isso.

Demais regras:

- O `.gitignore` bloqueia **qualquer** nome contendo `secrets` ou
  `credentials`, em qualquer diretorio e em qualquer combinacao de
  maiusculas, mais backups (`*.bak`, `*.old`, `*.orig`, `*.save`, `*~`),
  `.env` e chaves (`*.pem`, `*.key`). A unica excecao e
  `secrets.example.h`, que nao tem valores reais.

  As regras sao amplas porque duas versoes mais estreitas ja falharam em
  teste: `src/secrets.h` deixava passar `secrets.h.bak`, e `*secrets*`
  deixava passar `SECRETS.h` — o gitignore diferencia maiusculas. Dai as
  classes de caractere (`*[Ss][Ee][Cc]...`) no arquivo: sao feias, mas
  cobrem as 128 combinacoes de caixa.

- O `.gitignore` so protege arquivo **nao rastreado**. Se algo ja foi
  commitado, continua no historico mesmo depois de entrar na lista.
- O template `secrets.example.h` nunca recebe valor real. Ele carrega os
  placeholders `PREENCHER_*`, e nao string vazia: o `static_assert` rejeita
  os dois, mas ate entao o template usava `""` e a compilacao passava,
  gerando firmware com credenciais em branco.
- Antes do primeiro push, conferir o que realmente seria publicado:
  ```bash
  git add -A && git status --porcelain
  ```
  Devem aparecer apenas: `.gitignore`, `README.md`, `platformio.ini`,
  `src/main.cpp`, `src/secrets.example.h` e `.vscode/extensions.json`.
- Se `secrets.h` for commitado por acidente, tratar a senha como vazada:
  **trocar a senha do Wi-Fi**. Remover o arquivo num commit seguinte nao
  resolve — o valor continua no historico do git.
- Quem tem o firmware tem a senha da rede. Se o ESP32 for emprestado,
  descartado ou vendido, apagar a flash (`esptool.py erase_flash`).
- O firmware chama `WiFi.persistent(false)`. O padrao do core e `true`, o
  que guardaria a senha tambem na NVS — uma segunda copia, sem utilidade
  aqui, ja que as credenciais vem compiladas via `secrets.h`.

  **A chamada precisa vir antes de `WiFi.mode()`**, e isso nao e obvio:
  `persistent()` so grava uma flag interna. Quem age sobre ela e
  `wifiLowLevelInit()`, que roda uma unica vez (protegida por
  `lowLevelInitDone`) e e disparada justamente pelo `WiFi.mode()`. Chamar
  `persistent(false)` depois do `mode()` nao tem efeito nenhum — o
  storage ja ficou em NVS e nao ha segunda chance. Quem reordenar o
  `setup()` desfaz a protecao sem nenhum aviso do compilador.

---

## Como gravar no ESP32

### 1. Preparar o firmware

```bash
cd ~/Documentos/Projetos/melchior-wol
cp src/secrets.example.h src/secrets.h   # se ainda nao existir
$EDITOR src/secrets.h                     # preencher SSID e senha reais
```

### 2. Conectar a placa

Cabo USB **de dados** (nao so de carga) entre ESP32 e notebook.
Verificar se o Linux reconheceu:

```bash
ls /dev/ttyUSB* /dev/ttyACM*   # tipicamente /dev/ttyUSB0
dmesg | tail                    # em caso de duvida
```

Se aparecer mas der permissao negada no upload:

```bash
sudo usermod -aG dialout $USER
# fazer logout/login para o grupo entrar em vigor
```

### 3. Build e upload

**Pela extensao PlatformIO no VSCodium** — barra lateral, icone do
PlatformIO:

1. **Project Tasks → esp32dev → General → Build**
2. Se compilar sem erro: **Project Tasks → esp32dev → General → Upload**
3. **Project Tasks → esp32dev → General → Monitor** (serial 115200 baud)

**Pela linha de comando** — equivalente, util para script:

```bash
cd ~/Documentos/Projetos/melchior-wol
pio run                 # compila
pio run -t upload       # compila e grava na placa
pio device monitor      # abre a serial a 115200
```

Se a placa nao for detectada sozinha, informar a porta:

```bash
pio run -t upload --upload-port /dev/ttyUSB0
```

Algumas placas exigem segurar o botao **BOOT** durante o inicio do
upload (quando aparece `Connecting....`), soltando depois.

Referencia de tamanho de um build limpo (esp32dev, 4 MB flash):
RAM 13.8%, Flash 56.4%.

Com o melchior ja ligado, a serial mostra:

```
=== Sentinela de Wake-on-LAN do Melchior ===
Alvo do ping: 192.168.1.100
MAC para o WoL: AA:BB:CC:DD:EE:FF
Intervalo de monitoramento: 5 min

Wi-Fi OK. IP do ESP32: 192.168.1.xxx
Verificando o melchior (heap livre: 268412 bytes)... ONLINE.
Verificando o melchior (heap livre: 268408 bytes)... continua online.
```

O **heap livre** aparece em todo ciclo de proposito. O aparelho cria e
destroi uma sessao de ping a cada 5 min, para sempre (~105 mil por ano),
e o fonte do `esp_ping` nao e distribuido — so o `liblwip.a` compilado.
Nao da para descartar vazamento por inspecao de codigo. Com o numero no
log, a duvida vira observavel:

- oscila em torno de um valor estavel -> sem vazamento
- cai de forma continua ao longo de dias -> ha vazamento

Deixar o monitor serial aberto por algumas horas depois de gravar e
suficiente para tirar essa duvida.

A partir dai repete `continua online` a cada 5 minutos. Com o melchior
desligado:

```
Verificando o melchior... SEM RESPOSTA.
Enviando Wake-on-LAN (tentativa 1) para AA:BB:CC:DD:EE:FF
  3/3 pacotes enviados.
Aguardando 90s antes de verificar de novo.
Verificando o melchior... ONLINE.
Subiu depois de 1 tentativa(s) de Wake-on-LAN.
```

Se o Wi-Fi nao conectar em 30 segundos, o firmware desiste, explica o
motivo provavel e reinicia sozinho para tentar de novo:

```
FALHA: nao conectou no Wi-Fi dentro do timeout.
Confira SSID/senha em src/secrets.h.
Confira tambem se a rede e 2.4 GHz (o ESP32 nao fala 5 GHz).
Reiniciando em 10s para tentar de novo...
```

### Outras mensagens da serial

| Mensagem | Significado |
|---|---|
| `Wi-Fi desconectado. Reconectando...` | Normal e transitorio. Toda rede cai de vez em quando; o firmware reconecta sozinho. Preocupante so se aparecer a cada ciclo. |
| `[erro] nenhum magic packet saiu. Problema de rede no ESP32.` | O `sendto()` falhou nas tres tentativas. Nao e o melchior: e a pilha de rede do proprio ESP32. Costuma vir junto de instabilidade de Wi-Fi. |
| `[erro] nao foi possivel criar a sessao de ping` | **A mais importante das tres.** Significa que o `esp_ping_new_session()` nao conseguiu alocar. A causa provavel e falta de heap — ou seja, e o sintoma que confirmaria o vazamento discutido acima. Se aparecer, conferir o valor de heap livre nos ciclos anteriores: ele deve estar baixo e em queda. |

### Erro de compilacao esperado

Se as credenciais nao estiverem utilizaveis, o build **falha de
proposito**, antes de gerar firmware. Sao duas checagens por campo,
porque pegam descuidos diferentes:

| Situacao | Mensagem |
|---|---|
| Copiou o template e nao editou | `Preencha SECRET_WIFI_SSID em src/secrets.h antes de compilar.` |
| Idem, campo da senha | `Preencha SECRET_WIFI_PASSWORD em src/secrets.h antes de compilar.` |
| Apagou o SSID e deixou `""` | `SECRET_WIFI_SSID esta vazio em src/secrets.h. Nao existe rede sem nome.` |
| Apagou a senha e deixou `""` | `SECRET_WIFI_PASSWORD esta vazia em src/secrets.h. Veja o comentario acima se a rede for aberta.` |

Isso e intencional: evita gravar uma placa com credencial invalida e so
descobrir depois, olhando a serial — onde o sintoma seria um ciclo de
reinicio sem explicacao.

A checagem de string vazia foi acrescentada depois de uma auditoria
mostrar que o caminho ensinado por este mesmo README
(`cp src/secrets.example.h src/secrets.h`) compilava sem reclamar: o
template usava `""`, e a unica checagem existente comparava contra
`PREENCHER_*`. String vazia nao e igual ao placeholder, entao o build
passava e produzia firmware com SSID e senha em branco.

**Rede aberta.** Senha vazia e legitima apenas nesse caso. Para permitir,
comentar o `static_assert` correspondente no `main.cpp` — ha um
comentario no lugar explicando. Vale lembrar que rede aberta e escolha
ruim para um aparelho que fica ligado permanentemente.

### 4. Verificar que o melchior acordou

Com o melchior desligado (mas com cabo de rede e energia conectados):

```bash
ping -c 5 192.168.1.100
```

Se nao responder, o problema costuma ser no host, nao no ESP32:

- BIOS/UEFI: opcao "Wake on LAN" ou "Power on by PCI-E" habilitada.
- Ubuntu: NIC precisa ficar armada no shutdown. Conferir com
  `sudo ethtool <interface>` — a linha `Wake-on:` deve mostrar `g`.

### 5. Alimentacao em producao

Um carregador USB 5 V comum resolve. O dispositivo foi feito para ficar
ligado **permanentemente** — e essa a funcao dele: vigiar o melchior e
reagir sozinho. Nao precisa de intervencao depois de gravado.

Se faltar energia, ao voltar ele reinicia, pinga e retoma o ciclo. Se o
melchior estiver desligado nesse momento, ele acorda.

---

## Primeira gravacao: o que observar

Checklist para a primeira meia hora com o monitor serial aberto, antes de
deixar o aparelho sozinho na tomada. Em ordem de probabilidade real.

### 1. UFW no melchior bloqueando ICMP  (o mais provavel)

A sentinela decide tudo pelo ping. Se o firewall do melchior descartar
ICMP echo, ela le "desligado" com o servidor ligado e passa a mandar
Wake-on-LAN para sempre numa maquina que ja esta no ar. Os pacotes sao
inofensivos, mas o diagnostico fica invertido e o log vira ruido.

**Sintoma:** `SEM RESPOSTA` a cada ciclo, mesmo com o melchior ligado e
respondendo `ping` de outra maquina.

**Conferir no melchior:**

```bash
sudo ufw status verbose
ping -c 3 192.168.1.100      # de outra maquina da rede
```

### 2. Wake-on-LAN nao armado  (o mais provavel para o wake falhar)

O magic packet pode sair perfeito e o melchior nao acordar. Isso e
configuracao do host, nao do firmware.

**Conferir no melchior, antes de desligar:**

```bash
sudo ethtool <interface> | grep Wake-on    # precisa mostrar 'g'
```

Se mostrar `d`, o WoL esta desarmado. Ativar com `sudo ethtool -s
<interface> wol g`, e tornar persistente (o ajuste nao sobrevive a
reboot sozinho). Conferir tambem a BIOS/UEFI: "Wake on LAN" ou
"Power on by PCI-E" habilitado.

### 3. MAC da interface errada

O `TARGET_MAC` precisa ser o da interface **cabeada**. Se por engano for
o do Wi-Fi, o magic packet vai para um endereco que nao existe no
segmento cabeado e nada acontece.

```bash
ip link    # no melchior, conferir qual MAC pertence a qual interface
```

### 4. Heap caindo ao longo do tempo  (o unico que so o hardware responde)

O firmware imprime o heap livre a cada ciclo. Esta e a unica duvida do
projeto que nenhuma analise estatica resolve: o `esp_ping` cria e destroi
uma sessao a cada 5 min, para sempre, e o fonte nao e distribuido.

**Como ler:** anotar o valor no primeiro ciclo e comparar depois de umas
horas. Oscilando em torno de um patamar, nao ha vazamento. Caindo de forma
continua, ha — e o sintoma final seria a mensagem
`[erro] nao foi possivel criar a sessao de ping`.

### 5. Wi-Fi

Rede 2.4 GHz e sinal suficiente no local onde o ESP32 vai ficar. O
firmware avisa explicitamente se nao conectar em 30 s.

### Ja verificado, nao precisa observar

O firmware deixa `on_ping_success` e `on_ping_timeout` como `NULL`,
definindo so `on_ping_end`. Chegou a ser levantado se o ESP-IDF invocaria
um ponteiro nulo e travaria no primeiro ping.

**Nao trava.** Desmontando o `ping_sock.c.obj` do `liblwip.a` (o fonte nao
e distribuido, mas o binario esta no toolchain), os tres callbacks sao
carregados da struct da sessao e cada um passa por um `beqz` que desvia da
chamada indireta quando o ponteiro e nulo:

```
l32i   a4, a2, 112    ; carrega o ponteiro do callback
beqz   a4, <adiante>  ; se nulo, pula a chamada
callx8 a4             ; so entao chama
```

Ficou registrado aqui para nao virar suspeita de novo.

## Alternativas avaliadas e nao adotadas

Registradas aqui e no `main.cpp` para nao serem reconsideradas do zero
mais tarde.

### Retentar o Wi-Fi sem reiniciar

**Proposito.** Se o roteador ficar fora do ar por muito tempo, o
`ESP.restart()` do `garantirWiFi()` vira um ciclo de reboot a cada ~40 s
(30 s de timeout + 10 s de espera), indefinidamente. A alternativa seria
insistir no Wi-Fi no proprio laco, sem nunca reiniciar.

**Solucao proposta.** Trocar `println` + `delay` + `ESP.restart()` por um
rearme do timeout seguido de `WiFi.begin()` e `continue`. O trecho exato
esta comentado no `main.cpp`, junto do `ESP.restart()`.

**Por que esta inativa.**

1. O restart limpa fragmentacao de heap acumulada. Num aparelho que roda
   por meses sem parar, isso e vantagem real: o reboot periodico e
   higiene, nao efeito colateral.
2. O custo do reboot era desgaste de flash, porque cada `WiFi.begin()`
   escrevia na NVS. Com `WiFi.persistent(false)` no `setup()`, esse custo
   deixou de existir.
3. O estado perdido no reboot (`estado`, `tentativasWol`) e barato de
   reconstruir: o primeiro ciclo apos o boot ja pinga e redescobre se o
   melchior esta no ar.

O ciclo de reboot nao e defeito a corrigir — e o comportamento de
recuperacao escolhido.

## Estado conhecido

- **Sem teste automatizado.** O firmware e simples o suficiente para
  validar por serial monitor.
- **Sem OTA.** Cada mudanca exige cabo USB.
- **Sem historico.** O estado so aparece na serial, ao vivo. Nada e
  gravado: desconectou o monitor, perdeu o log.
- **Nao distingue "desligado" de "inalcancavel".** Se o melchior estiver
  ligado mas isolado (cabo solto, switch fora, firewall bloqueando ICMP),
  a sentinela le como desligado e manda WoL. Sao pacotes inofensivos, mas
  o diagnostico na serial fica enganoso.
- **Credencial em texto claro no binario.** Inerente ao Arduino. Ver a
  secao Seguranca.
- **Sem alarme.** Se o melchior nunca subir, o ESP32 tenta para sempre em
  silencio. Nao ha notificacao para fora.

## Verificacoes ja feitas

- Compila limpo para `esp32dev` (RAM 13.8%, Flash 56.4%), sem warnings.
- Estrutura do magic packet validada byte a byte contra a spec do
  Wake-on-LAN: 102 bytes, 6x `0xFF` + 16 repeticoes do MAC, sem lacuna
  nem estouro de buffer.
- Conversao de `IPAddress` para o `ip4_addr_t` do lwIP conferida contra
  as macros reais (`LWIP_MAKEU32` + `PP_HTONL`): os dois lados produzem
  `0xC312A8C0` para 192.168.1.100. Um erro de ordem de bytes aqui faria
  a sentinela pingar outro host sem avisar.
- Maquina de estados simulada em quatro cenarios (ja ligado no boot;
  sobe com 1 WoL; resiste a 5 WoL; cai durante o monitoramento). As
  transicoes e os intervalos batem com o especificado.
- Broadcast para `255.255.255.255` funciona sem `setsockopt(SO_BROADCAST)`
  porque o lwIP do ESP-IDF compila com `IP_SOF_BROADCAST = 0`.
- `udp.begin()` nao e necessario: `beginPacket()` cria o socket sozinho.
- Toda sessao de ping e encerrada com `esp_ping_delete_session()`. Sem
  isso haveria vazamento de memoria a cada 5 minutos, indefinidamente.
- O socket UDP e fechado (`udp.stop()`) antes de cada reconexao de Wi-Fi.
  O `WiFiUDP` reaproveita o mesmo socket para sempre depois de criado
  (`beginPacket()` retorna cedo se `udp_server != -1`), entao sem isso o
  magic packet sairia por um socket da sessao de rede anterior.
- Compila sem nenhum aviso sob `-Wall -Wextra`. Verificado com um teste
  de controle (uma variavel nao usada plantada de proposito) para
  confirmar que os avisos estavam mesmo ativos, e nao silenciados.
- Constantes e mensagens da serial deste README conferidas contra o
  codigo, uma a uma.
- Teste adversarial do `.gitignore` com 28 vetores: copias do `secrets.h`
  em variantes de sufixo (`.bak`, `_backup`, `.old`, `.orig`, `.save`,
  `copy`, `~`, `.swp`), de caixa (`SECRETS.h`, `Secrets.h`, `SeCrEtS.h`),
  em subdiretorios, mais `credentials.h`, `.env`, `.pem` e `.key`. Cada
  uma criada com uma senha real dentro; nenhuma foi versionada, e o
  `secrets.example.h` continuou publicado.
- Simulacao de publicacao com credencial real preenchida: nada de
  sensivel entra no `git add -A`.
