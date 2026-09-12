# Sentinela de Wake-on-LAN

Dispositivo autonomo baseado em ESP32 que vigia uma maquina da rede e a
liga por Wake-on-LAN quando ela para de responder.

Fica ligado permanentemente numa tomada. A cada 5 minutos pinga o alvo;
enquanto responder, so observa. Quando para de responder, envia o magic
packet, espera o tempo de boot e confere de novo, repetindo ate a maquina
subir. Serve uma pagina de status HTTP para consulta pelo navegador.

Nao depende de servidor, servico externo ou nuvem: a decisao inteira
acontece no proprio aparelho, dentro da rede local.

Toda a configuracao vive em `src/secrets.h`, um arquivo de **oito
campos** que nao vai para o git. Tres deles descrevem o alvo — nome, IP e
MAC — e trocar esses tres aponta a sentinela para outra maquina da mesma
rede sem alterar codigo; os outros cinco descrevem a rede Wi-Fi e o
endereco do proprio ESP32. Na instalacao que originou o projeto, o alvo e
um notebook reaproveitado rodando Ubuntu Server como servidor de homelab;
os exemplos deste README usam esse caso.

```
        ┌──────────────────────────────────────────────┐
        │                                              │
        v                                              │
   [ ping 192.168.X.Y ]                             │
        │                                              │
        ├── respondeu ──────> ONLINE ── espera 5min ───┘
        │
        ├── nao respondeu ──> [ magic packet para AA:BB:CC:DD:EE:FF ]
        │                            │
        │                     espera 90s (boot do Ubuntu)
        │                            │
        │                     volta a pingar. Se ainda nao subiu,
        │                     repete indefinidamente ate ligar. A
        │                     partir da 4a tentativa, a cada 5 min.
        │
        └── falhou aqui dentro ──> avisa na serial e REINICIA
                                   (nao e diagnostico do alvo)
```

O terceiro caminho e o que separa "o alvo nao respondeu" de "a
checagem nao funcionou". Se a sessao de ping nao pode ser criada,
nao arranca, ou termina sem resposta da tarefa interna, o problema e do
ESP32 e nao do alvo. Concluir "alvo desligado" nesses casos faria a
sentinela mandar Wake-on-LAN a toa num servidor que talvez esteja no ar,
e o log culparia o alvo por um defeito local. Por isso o firmware avisa e
reinicia, que e o que efetivamente cura essas tres falhas. Detalhe de
cada uma na tabela de mensagens da serial, mais adiante.

O projeto roda exclusivamente em ESP32 fisico. Nao ha simulador no
fluxo: build e teste acontecem na propria placa, via USB e serial
monitor.

### Por que ping por IP e nao pelo MAC

Nao da para monitorar so pelo MAC. ICMP (ping) e camada 3 e exige um
endereco IP — nao existe "pingar um MAC". O equivalente em camada 2
seria ARP, mas ARP e uma consulta indexada por IP ("quem tem
192.168.X.Y?"), entao o IP continua sendo necessario.

Ha um agravante: NICs armadas para Wake-on-LAN ficam parcialmente
energizadas com a maquina desligada, e algumas respondem ARP nesse
estado. ARP reportaria "ligado" para um servidor desligado — o oposto
do que se quer detectar. ICMP exige a pilha de rede do SO no ar, que e
exatamente a condicao a verificar.

O IP do alvo precisa ser fixo. No caso concreto do projeto, e fixado por
netplan no proprio servidor, sem reserva de DHCP no roteador.

---

## Estrutura de arquivos

Versionado (vai para o GitHub):

```
sentinela-wol/
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
├── compile_commands.json     # gerado pelo PlatformIO; cheio de caminhos /home/<usuario>
├── Claude outputs/           # relatorios de sessao de trabalho, nao fazem parte do projeto
├── .vscode/c_cpp_properties.json, launch.json
└── .claude/settings.local.json
```

O que cada arquivo do `src/` faz:

| Arquivo | Papel |
|---|---|
| `main.cpp` | Firmware. Le do `secrets.h` os **oito** campos da instalacao: as duas credenciais de Wi-Fi, os tres do alvo (`SECRET_ALVO_NOME`, `SECRET_ALVO_MAC`, `SECRET_ALVO_IP`) e os tres de endereco do proprio ESP32 (`SECRET_ESP32_IP`, `SECRET_GATEWAY_IP`, `SECRET_MASCARA_REDE`). |
| `secrets.example.h` | Template publicavel. Contem os placeholders `PREENCHER_*`, nunca valores reais. Serve de referencia para quem clonar o repo. |
| `secrets.h` | Dados reais: rede Wi-Fi, alvo e endereco do proprio ESP32. Esta no `.gitignore`. Se nao existir, criar com `cp src/secrets.example.h src/secrets.h`. |

---

## Configuracao

**Tudo que e especifico da instalacao vive em `src/secrets.h`. O
`src/main.cpp` nunca precisa ser editado para instalar a sentinela em
outra rede ou apontar para outra maquina.**

Sao oito campos. Copie o template e preencha:

```bash
cp src/secrets.example.h src/secrets.h
$EDITOR src/secrets.h
```

| Campo | O que e | Onde descobrir | Trava |
|---|---|---|---|
| `SECRET_WIFI_SSID` | Nome da rede Wi-Fi. Precisa ser **2.4 GHz** — o ESP32 nao fala 5 GHz. | Lista de redes do celular. Se o roteador anuncia o mesmo nome nas duas bandas, confirmar que a 2.4 esta ativa. | sim |
| `SECRET_WIFI_PASSWORD` | Senha da rede. | — | sim |
| `SECRET_ALVO_NOME` | Nome da maquina vigiada. So aparece nas mensagens da serial e na pagina. | Escolha sua. | sim |
| `SECRET_ALVO_MAC` | MAC da interface **cabeada** do alvo. E o endereco que o magic packet acorda. | No alvo: `ip link` (Linux) ou `ipconfig /all` (Windows). | sim |
| `SECRET_ALVO_IP` | IP do alvo. E quem o ping consulta. Precisa ser fixo. | No alvo: `ip addr`. | sim |
| `SECRET_ESP32_IP` | IP que o proprio ESP32 assume. Precisa estar **fora da faixa de DHCP** do roteador. | Painel do roteador, na configuracao de DHCP. | sim |
| `SECRET_GATEWAY_IP` | Endereco do roteador. Serve de gateway e de DNS. | `ip route \| grep default` | sim |
| `SECRET_MASCARA_REDE` | Mascara da rede local. | Quase sempre `255, 255, 255, 0`. | **so quantidade** |

### Por que a mascara e a unica sem trava de valor

As outras sete rejeitam o valor de exemplo em tempo de compilacao: se
voce copiar o template e esquecer de editar, o build falha com uma
mensagem dizendo qual campo trocar.

Com a mascara isso nao funciona. `255.255.255.0` e a mascara legitima da
grande maioria das redes domesticas — rejeitar esse valor daria falso
positivo em quase toda instalacao real, e obrigaria a inventar um valor
"nao-exemplo" que provavelmente estaria errado. Nela so a quantidade de
octetos e conferida.

As travas de MAC e IP conferem duas coisas: a quantidade de bytes (um MAC
com cinco bytes nao compila, em vez de ter o sexto preenchido com zero em
silencio) e a diferenca em relacao ao valor de exemplo. Esses dois campos
ganharam trava porque sao os de erro mais dificil de diagnosticar: MAC
errado nao gera erro nenhum — o magic packet sai perfeito para um
endereco que nao existe — e IP errado faz a sentinela concluir que o alvo
vive desligado.

### Toolchain

O `platformio.ini` fixa `platform = espressif32@7.1.1` em vez de deixar
`espressif32` solto. Sem isso, quem clonar o repo daqui a um ano pega
outra versao do toolchain e pode nao compilar. Para atualizar de
proposito, subir o numero e recompilar.

Nao ha `lib_deps`: o ping usa a API nativa do ESP-IDF (`ping_sock.h`),
que ja vem no `liblwip.a` do core. O projeto nao depende de nenhuma
biblioteca externa.

Tambem esta ligado o `monitor_filters = esp32_exception_decoder`, que
traduz backtrace de crash em `arquivo:linha` em vez de enderecos crus.
Util justamente porque os tres caminhos de falha da checagem reiniciam o
aparelho: se algum dia o reinicio vier de um panic em vez do
`ESP.restart()`, a diferenca aparece na serial.

### Tempos

Constantes no topo do `main.cpp`:

| Constante | Valor | Papel |
|---|---|---|
| `INTERVALO_MONITORAMENTO_MS` | 5 min | Espera entre checagens com o alvo no ar. |
| `ESPERA_POS_WOL_MS` | 90 s | Tempo dado ao alvo para subir a rede depois do WoL. No caso do projeto, um Ubuntu Server em disco mecanico. |
| `INTERVALO_RETENTATIVA_MS` | 5 min | Espera entre WoL depois das tentativas rapidas. |
| `TENTATIVAS_RAPIDAS` | 3 | Quantos WoL a 90 s antes de espacar para 5 min. |
| `PINGS_POR_CHECAGEM` | 3 | Basta 1 resposta para considerar online. |

O escalonamento existe para nao martelar a rede quando o alvo esta
fisicamente fora da tomada. Ele nunca desiste — so passa a insistir mais
devagar.

Se o alvo demorar mais de 90 s para responder ao ping depois de
ligar, o firmware manda um WoL a mais sem necessidade. Isso e inofensivo
(magic packet em maquina ligada nao faz nada), mas se incomodar, aumentar
`ESPERA_POS_WOL_MS`.

### Broadcast

Fixado em `255.255.255.255` (limited broadcast). Nao depende da faixa de
IP da rede local — trocar de roteador ou provedor nao exige mexer aqui.

---

## Pagina de status

O firmware sobe um servidor HTTP na porta 80. Com o `SECRET_ESP32_IP`
preenchido como `192.168.X.Z`, a pagina responde em:

```
http://192.168.X.Z/
```

O endereco tambem aparece na serial no boot, na linha
`Pagina de status: http://`.

E preciso estar **na mesma rede local** do ESP32. Nao ha acesso de fora:
o aparelho nao publica nada na internet, nao usa nuvem e nao abre tunel
nenhum.

### Somente leitura

A pagina nao tem botao, formulario nem link de acao. Nao da para forcar
um Wake-on-LAN, reiniciar o ESP32, mudar o intervalo ou alterar
configuracao alguma por ela. A unica rota registrada e `/`, e tudo que
ela faz e montar HTML a partir de variaveis em memoria. O que muda
comportamento esta no `secrets.h` e nas constantes do `main.cpp` — ou
seja, exige recompilar e regravar pela USB.

### Nao exponha na internet

**Sem redirecionamento de porta no roteador, sem UPnP, sem proxy reverso
publico.**

O motivo nao e a informacao que a pagina mostra. Quem ja esta na LAN
descobre IP e MAC de qualquer aparelho com um `arp -a`, e a pagina nao
revela senha nenhuma.

O motivo e outro: **um servidor web de dispositivo embarcado nao e feito
para aguentar trafego hostil.** Sao alguns KB de codigo num
microcontrolador de 320 KB de RAM, sem limite de taxa, sem timeout
agressivo, sem defesa contra requisicao malformada e sem nada que
sobreviva a uma varredura automatizada — e a internet varre qualquer
porta 80 aberta em questao de horas. O risco aqui nao e vazar dado: e a
sentinela parar de vigiar porque o servidor web dela travou o aparelho.

### O que ela mostra

| Bloco | Campo | O que e |
|---|---|---|
| topo | Estado e "ha ..." | `ONLINE`, `OFFLINE` ou `verificando`, e ha quanto tempo esta assim. Conta desde a ultima **transicao**, nao desde o boot. |
| Verificacao | Ultima | Ha quanto tempo foi o ultimo ping. |
| Verificacao | Proxima em | Quanto falta para a proxima. Mostra `agora` quando o ciclo ja deveria ter acontecido — o aparelho esta no meio de uma checagem. |
| Verificacao | WoL desde a ultima subida | Magic packets enviados desde a ultima vez que o alvo respondeu. Zera quando ele sobe. |
| Verificacao | WoL desde o boot | Total acumulado desde que o ESP32 ligou. Nao zera. |
| ESP32 | Firmware | A versao editada a mao e o momento da compilacao. Ver "Carimbo de versao". |
| ESP32 | Ligado ha | Tempo desde o ultimo boot. Ver a ressalva do reinicio de 24 h, adiante. |
| ESP32 | Heap livre | Memoria livre **neste instante**. |
| ESP32 | Minimo desde o boot | **O pior momento de memoria livre, nao o valor atual.** Ver abaixo. |
| ESP32 | IP / MAC | Do proprio ESP32, lidos da pilha de rede — o que ele de fato esta usando. |
| ESP32 | Reinicios | Quantos desde a ultima queda de energia, e o motivo do ultimo. |
| Alvo | IP / MAC | Os valores compilados, vindos do `secrets.h`. Serve para conferir na hora se o firmware gravado e o que se pensa que e. |
| Ocorrencias | lista | Ate 20 eventos, do mais recente para o mais antigo. |

**"Minimo desde o boot" merece atencao** porque e facil ler errado. Nao e
a leitura atual: e o menor valor que o heap livre ja atingiu desde que o
aparelho ligou (`ESP.getMinFreeHeap()`). Ele so desce, nunca sobe. E ele
que responde a pergunta que importa — "em algum momento chegou perto do
fim?". O "Heap livre" da linha de cima pode estar confortavel agora e ter
havido um aperto ha seis horas; so o minimo mostra isso.

### Atualizacao automatica

A pagina se recarrega sozinha a cada 10 segundos, por
`<meta http-equiv="refresh">`. **Nao ha JavaScript e nao ha recurso
externo** — nenhuma fonte, folha de estilo ou biblioteca vinda de CDN.

Isso e deliberado: a pagina precisa abrir com a internet fora, que e
exatamente quando alguem vai querer olhar o estado do servidor de casa.
Uma pagina que dependesse de `<script src="https://...">` ficaria em
branco justamente na hora util.

O HTML tambem e enviado em blocos (chunked), montado num buffer fixo de
320 bytes, nunca numa `String`. Com refresh de 10 s sao ~8.600
requisicoes por dia; montar a pagina inteira em heap a cada uma daria
dezenas de milhares de alocacoes diarias no aparelho que o resto do
projeto trabalha para manter sem fragmentacao.

---

## IP fixo do ESP32

O aparelho assume um endereco fixo em vez de pedir um por DHCP. Sem isso
o IP muda sem aviso e a pagina de status "some" justamente quando alguem
precisa dela.

Tres campos do `secrets.h` definem isso:

| Campo | Exemplo | Papel |
|---|---|---|
| `SECRET_ESP32_IP` | `192, 168, X, Z` | O endereco que o ESP32 assume. |
| `SECRET_GATEWAY_IP` | `192, 168, X, 1` | O roteador. Serve de gateway e tambem de DNS. |
| `SECRET_MASCARA_REDE` | `255, 255, 255, 0` | Mascara da rede local. |

A configuracao e aplicada dentro de `garantirWiFi()`, e nao so no
`setup()`. Isso e de proposito: assim vale tambem em toda reconexao, e o
endereco nao depende da ordem das chamadas na inicializacao.

### A armadilha: a faixa de DHCP

**O IP escolhido precisa estar FORA da faixa que o roteador distribui por
DHCP.**

Se estiver dentro dela, o roteador nao tem como saber que aquele endereco
esta ocupado — o ESP32 nunca pediu nada a ele — e mais cedo ou mais tarde
entrega o mesmo endereco a outro aparelho. Os dois passam a responder
pelo mesmo IP e somem da rede **de forma intermitente**: as vezes
funciona, as vezes nao, dependendo de quem respondeu ARP por ultimo. E
dos sintomas mais chatos de diagnosticar, porque nao aparece erro em
lugar nenhum — nem no ESP32, nem no roteador, nem no outro aparelho.

Roteador domestico tipico distribui de `.100` a `.200`, e ai qualquer
coisa abaixo de `.100` serve. Alguns distribuem de `.2` a `.254`: nesse
caso nao sobra faixa livre, e e preciso encurtar o intervalo do DHCP no
painel antes de fixar qualquer endereco.

**Como conferir o endereco do roteador:**

```bash
ip route | grep default        # o IP do roteador vem depois de "via"
```

**Como conferir a faixa de DHCP:** so no painel do roteador. Abrir o IP
do gateway no navegador e procurar por "DHCP", "Servidor DHCP" ou "LAN
Settings". O nome do campo varia entre fabricantes: "faixa de
enderecos", "pool", "start/end address".

### Voltar para DHCP

Comentar uma unica linha no `main.cpp`, dentro do bloco delimitado por
`IP FIXO - inicio do bloco` / `fim do bloco`, em `garantirWiFi()`:

```cpp
// WiFi.config(ESP32_IP, GATEWAY_IP, MASCARA_REDE, GATEWAY_IP);
```

O aparelho passa a pegar endereco do roteador. O IP obtido aparece na
serial em duas linhas — `Wi-Fi OK. IP do ESP32:` e
`Pagina de status: http://` — que nesse modo continuam corretas, porque
as duas saem de `WiFi.localIP()` depois de a conexao existir, e nao da
constante. A pagina de status continua funcionando, so que num endereco
que pode mudar sem aviso.

### Reserva de DHCP, a alternativa

Quem preferir centralizar o controle no roteador pode deixar o ESP32 em
DHCP e criar uma **reserva** la, amarrando o endereco ao MAC do aparelho.
Para isso o firmware imprime o proprio MAC no boot:

```
MAC do ESP32: 3C:61:05:XX:XX:XX
```

Serve tambem para identificar o aparelho na lista de clientes do
roteador, onde sem isso ele fica como mais um dispositivo sem nome no
meio dos outros.

---

## Reinicio periodico de higiene

A cada 24 horas de funcionamento o aparelho se reinicia sozinho.

A checagem esta no inicio de cada ciclo do `loop()`, nao num temporizador
independente. Na pratica o reinicio cai no **primeiro ciclo depois das
24 h** — algo entre 24h00 e 24h05, porque com o alvo no ar cada ciclo
dura 5 minutos. Nao e um horario exato, e nao precisa ser.

A serial anuncia:

```
Reinicio periodico de higiene (24h de funcionamento).
```

### E defesa cega, nao diagnostico

Isto nao corrige um problema conhecido. Existe contra degradacao que o
codigo **nao tem como perceber sozinho**.

Dois casos motivam. O primeiro e fragmentacao de heap: o total livre pode
continuar alto enquanto ja nao existe nenhum bloco contiguo grande o
suficiente, e nao ha como checar isso de dentro com confianca.

O segundo e mais grave — a pilha de Wi-Fi entrar num estado em que
reporta `WL_CONNECTED` sem trafego real passar. Nesse estado nada falha
visivelmente: `garantirWiFi()` ve a conexao como boa e nao reconecta, o
ping falha honestamente porque nao ha rede, a sentinela conclui "alvo
desligado" e passa a mandar Wake-on-LAN para sempre num servidor que
esta ligado o tempo todo. Nenhuma das tres mensagens de erro da checagem
aparece, porque nenhuma delas descreve esse caso. O aparelho continua
"funcionando", so que cego — e nao ha como distinguir isso de um alvo
realmente fora do ar sem sair do proprio ESP32.

### Nao ha condicao de estado, de proposito

O reinicio acontece mesmo com o alvo offline no meio das tentativas de
Wake-on-LAN. Isso foi decidido, nao esquecido.

Condicionar o reinicio a "so quando estiver tudo calmo" criaria
exatamente o caminho que a defesa existe para cobrir: um aparelho
degradado que nunca se recupera justamente por estar ocupado. No cenario
do Wi-Fi fantasma acima, a sentinela ficaria permanentemente em OFFLINE
mandando WoL — portanto nunca "calma" — e nunca reiniciaria.

O custo de reiniciar no meio de uma sequencia e pequeno: depois do boot o
primeiro ciclo pinga em ~30 s, contra um intervalo de retentativa de
5 min.

### O custo

Cerca de **30 segundos de cegueira por dia** — boot, conexao de Wi-Fi e
primeiro ping. Sao 0,03% do tempo. Se o alvo cair exatamente nessa
janela, a deteccao atrasa um ciclo.

### O efeito colateral

**O tempo de funcionamento e o historico na pagina nunca passam de
24 horas.** Nao ha defeito nisso: o "Ligado ha" mostra o tempo desde o
ultimo reinicio, e as ocorrencias vivem em RAM comum, que o reinicio
apaga. Quem abrir a pagina esperando um historico de semanas vai achar
que algo se perdeu — nao se perdeu, nunca esteve la.

O contador de reinicios e o motivo do ultimo sao a excecao: sobrevivem,
pelo mecanismo da proxima secao.

---

## Historico de ocorrencias e contadores de reinicio

A pagina de status mostra ate **20 ocorrencias**, da mais recente para a
mais antiga, com "ha quanto tempo" ao lado de cada uma.

### Vetor de tamanho fixo, e por que

O historico e um vetor circular de 20 posicoes, com texto de 56 bytes
cada, reservado uma vez na inicializacao e imutavel dali em diante. Sem
`String`, sem `new`, sem `malloc`, sem `std::vector`. Quando a 21a
ocorrencia chega, ela sobrescreve a mais antiga.

A razao e a mesma que guia o resto do firmware: este aparelho fica meses
ligado registrando eventos. Texto de tamanho dinamico neste caminho
significaria alocar e liberar blocos de tamanhos variados milhares de
vezes, que e a receita de fragmentacao de heap — exatamente o problema
que o projeto passou uma revisao inteira descartando. Um log bonito nao
paga reintroduzi-lo pela porta dos fundos.

O preco do tamanho fixo e o truncamento: uma mensagem que passe de 56
bytes e cortada em silencio pelo `snprintf`. Com nome de maquina curto
isso nao acontece.

### O criterio: so ocorrencias, nunca rotina

Entra no historico o que e **evento**: boot, conexao de Wi-Fi, queda e
volta do Wi-Fi, mudanca de estado do alvo, envio de Wake-on-LAN, falha de
envio, e o motivo de um reinicio anterior. Nao entra o tique de rotina —
o `continua online` de cada ciclo.

A primeira conexao e a reconexao sao registradas com textos diferentes
(`Conectando no Wi-Fi` e `Wi-Fi caiu - reconectando`). A distincao
importa: ate a versao anterior o boot gravava uma queda de Wi-Fi que
nunca tinha acontecido, e como o boot so era registrado depois, um
aparelho recem-ligado exibia os eventos em ordem enganosa — o boot
aparecia como o mais recente dos tres, acima da conexao que tinha
partido dele.

Isso e essencial, nao economia de memoria. Com o alvo no ar, um ciclo
acontece a cada 5 minutos. Se cada um gravasse uma linha, as 20 posicoes
se esgotariam em **100 minutos**, e qualquer ocorrencia real seria
empurrada para fora antes de alguem ter chance de ver. O historico existe
para responder "o que aconteceu de diferente", e uma lista de vinte
`continua online` nao responde nada.

### Contadores que sobrevivem ao reinicio

O historico vive em RAM comum, e o reinicio o apaga. Isso deixava um
buraco justamente no que mais importa: os eventos que **disparam** um
reinicio eram os unicos que nunca chegavam a aparecer na pagina, porque o
proprio reinicio que eles anunciavam os destruia.

Duas coisas foram para a **RTC RAM**, uma regiao pequena que o reset por
software nao zera:

- quantos reinicios ja houve;
- o motivo do ultimo, em texto.

No boot o firmware le as duas, imprime na serial e **reinjeta o motivo no
historico**, como uma ocorrencia `Reiniciou: <motivo>`. E por isso que a
pagina consegue dizer POR QUE o aparelho reiniciou, mesmo o reinicio
tendo apagado a memoria onde essa informacao estava.

```
Reinicios desde a ultima queda de energia: 3
Motivo do ultimo: reinicio periodico de higiene (24h)
```

**A RTC RAM nao sobrevive a queda de energia, e essa e a semantica
desejada.** Falta de luz nao e sintoma de defeito do aparelho: se o
contador continuasse somando entre apagoes, passaria a medir a
confiabilidade da rede eletrica em vez da saude do firmware. Tirar o
aparelho da tomada zera a contagem de proposito — dai o "desde a ultima
queda de energia" no texto.

Uma palavra magica gravada junto distingue dado nosso de lixo: depois de
um power-on a regiao vem com qualquer conteudo, e sem essa checagem o
firmware anunciaria um numero aleatorio de reinicios com um motivo
ilegivel.

Os motivos possiveis hoje sao cinco:

| Motivo | Origem |
|---|---|
| `Wi-Fi nao conectou dentro do prazo` | 30 s sem conectar, em `garantirWiFi()`. |
| `falha ao criar a sessao de ping` | `esp_ping_new_session()` recusou. |
| `falha ao iniciar a sessao de ping` | `esp_ping_start()` recusou. |
| `ping sem retorno dentro do prazo` | O callback do ping nunca veio. |
| `reinicio periodico de higiene (24h)` | O reinicio programado. Unico sem defeito envolvido. |

Todo reinicio passa por uma unica funcao, `reiniciar()`, que grava o
motivo antes de chamar `ESP.restart()`. Nao existe `ESP.restart()` solto
no arquivo — logo, nao existe reinicio sem motivo registrado.

---

## Carimbo de versao

O firmware se identifica em dois lugares, com a mesma informacao: na
serial, logo abaixo do cabecalho de boot, e na pagina de status, na
primeira linha do bloco ESP32.

```
Firmware 1.0, compilado em Sep 11 2026 16:45:12
```

Sao dois dados de natureza diferente:

| Parte | De onde vem | O que significa |
|---|---|---|
| `1.0` | `FIRMWARE_VERSAO`, no `main.cpp` | **Editada a mao** a cada versao significativa. Diz que versao se quis gravar. |
| `Sep 11 2026 16:45:12` | `__DATE__` e `__TIME__` | Substituidos pelo pre-processador. Diz quando este binario foi feito. |

Nao ha nada automatico atras do numero de versao — nem tag de git, nem
contador de build — e isso e deliberado. O aparelho e gravado por USB, de
um clone que pode estar em qualquer ponto do historico, as vezes com
alteracao nao commitada; um numero gerado automaticamente daria
impressao de rastreabilidade que nao existe. O numero diz a intencao, e o
carimbo de compilacao e o dado objetivo ao lado dele.

**A ressalva do `__DATE__`/`__TIME__`.** Os dois congelam no momento em
que o **`main.cpp`** e compilado, e so entao. Numa compilacao incremental
que nao recompile esse arquivo — mexeu so no `secrets.h`, por exemplo — o
carimbo antigo vai inteiro para dentro do binario novo, e a placa passa a
mentir sobre a propria idade justamente quando se esta tentando descobrir
qual firmware ela tem.

Por isso o build de validacao do projeto e **sempre do zero**:

```bash
rm -rf .pio/build && pio run
```

Para que serve na pratica: com o aparelho ja na tomada ha meses, abrir a
pagina e comparar o carimbo com o do ultimo build responde em dois
segundos a pergunta "esta placa tem a versao que eu acho que tem?" — sem
desmontar nada e sem cabo USB.

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
cd /caminho/para/sentinela-wol
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
cd /caminho/para/sentinela-wol
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
RAM 14,3% (46.776 B), Flash 60,1% (787.193 B).

Nos exemplos abaixo o `SECRET_ALVO_NOME` esta preenchido com
`servidor`, e MAC e IP aparecem como placeholders.

Com o alvo ja ligado, a serial mostra:

```
=== Sentinela de Wake-on-LAN ===
Firmware 1.0, compilado em Sep 11 2026 16:45:12
Alvo: servidor  192.168.X.Y  AA:BB:CC:DD:EE:FF
Intervalo de monitoramento: 5 min
MAC do ESP32: 3C:61:05:XX:XX:XX

Conectando no Wi-Fi...
.....
Wi-Fi OK. IP do ESP32: 192.168.X.Z
Pagina de status: http://192.168.X.Z
Verificando o servidor (heap livre: 268412 bytes)... ONLINE.
Verificando o servidor (heap livre: 268408 bytes)... continua online.
```

Duas coisas a notar na ordem desse bloco:

- **`Conectando no Wi-Fi...` e diferente de `Wi-Fi desconectado.
  Reconectando...`.** A mesma funcao (`garantirWiFi()`) trata a primeira
  conexao e as reconexoes, mas anuncia cada caso com o texto que
  corresponde. No boot so aparece o primeiro; o segundo significa que uma
  conexao que existia caiu. Os pontos que vem depois sao o progresso da
  tentativa.
- **`Pagina de status:` so sai depois de `Wi-Fi OK`**, e imprime o
  endereco que a interface de fato assumiu (`WiFi.localIP()`). Vale nos
  dois modos, IP fixo ou DHCP.

Depois de um reinicio, e so depois dele, aparecem mais duas linhas logo
abaixo do carimbo de versao:

```
=== Sentinela de Wake-on-LAN ===
Firmware 1.0, compilado em Sep 11 2026 16:45:12
Reinicios desde a ultima queda de energia: 3
Motivo do ultimo: reinicio periodico de higiene (24h)
Alvo: servidor  192.168.X.Y  AA:BB:CC:DD:EE:FF
```

Elas somem quando o aparelho e desligado da tomada — a contagem vive em
RTC RAM, que a queda de energia zera. Ver a secao de historico e
contadores.

O **heap livre** aparece em todo ciclo como termometro barato, nao
porque haja suspeita em aberto.

O aparelho cria e destroi uma sessao de ping a cada 5 min, para sempre
(~105 mil por ano), o que ja levantou a duvida de vazamento. Ela esta
encerrada: lendo o `ping_sock.c` do ESP-IDF, o `esp_ping_delete_session()`
nao libera nada na hora — so marca a sessao para encerrar. Quem libera e
a tarefa interna do ping, que espera com prazo de 1 segundo e, ao ver a
marca, sai devolvendo a memoria, o buffer do pacote ICMP, o socket e a si
mesma. Como o firmware espera 5 minutos ate a proxima verificacao, a
margem e de 300 para 1.

A partir dai repete `continua online` a cada 5 minutos. Com o alvo
desligado:

```
Verificando o servidor (heap livre: 268404 bytes)... SEM RESPOSTA.
Enviando Wake-on-LAN (tentativa 1) para AA:BB:CC:DD:EE:FF
  3/3 pacotes enviados.
Aguardando 90s antes de verificar de novo.
Verificando o servidor (heap livre: 268404 bytes)... ONLINE.
Subiu depois de 1 tentativa(s) de Wake-on-LAN.
```

<!--
EXEMPLO ANTIGO - DESATIVADO, mantido para auditoria.

Este era o texto deste bloco antes de o firmware passar a imprimir o
heap livre em cada ciclo. Ficou desatualizado no commit que adicionou a
instrumentacao de memoria: o bloco de cima (alvo ligado) foi
corrigido na epoca, este aqui passou despercebido e so apareceu numa
verificacao posterior que comparou mensagem por mensagem.

Nao foi apagado porque documenta que o formato do log mudou - quem
encontrar uma gravacao de serial antiga, sem o heap, sabe que e de uma
versao anterior e nao um defeito.

Verificando o servidor... SEM RESPOSTA.
Enviando Wake-on-LAN (tentativa 1) para AA:BB:CC:DD:EE:FF
  3/3 pacotes enviados.
Aguardando 90s antes de verificar de novo.
Verificando o servidor... ONLINE.
Subiu depois de 1 tentativa(s) de Wake-on-LAN.
-->


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
| `Firmware <versao>, compilado em <data> <hora>` | So no boot. Ver a secao "Carimbo de versao". |
| `Conectando no Wi-Fi...` | Primeira conexao do ciclo de vida atual — boot ou reinicio. Nada caiu. |
| `Wi-Fi desconectado. Reconectando...` | Uma conexao que **existia** caiu. Normal e transitorio: toda rede cai de vez em quando, e o firmware reconecta sozinho. Preocupante so se aparecer a cada ciclo. |
| `Wi-Fi OK. IP do ESP32: <ip>` | Conexao estabelecida. Com o IP fixo ativo, este endereco e o do `SECRET_ESP32_IP`; se divergir, o `WiFi.config()` nao esta valendo. |
| `MAC do ESP32: <mac>` | So no boot. O MAC do proprio aparelho, para reserva de DHCP no roteador e para identifica-lo na lista de clientes. |
| `Pagina de status: http://<ip>` | So no boot, **depois** de a rede existir. O endereco vem de `WiFi.localIP()`, entao e o real nos dois modos. |
| `Reinicios desde a ultima queda de energia: <n>` | So no boot, e so se houve reinicio. Vem da RTC RAM; zera quando falta energia. |
| `Motivo do ultimo: <texto>` | Acompanha a linha acima. Um dos cinco motivos da tabela da secao de historico. |
| `Reinicio periodico de higiene (24h de funcionamento).` | O reinicio programado de 24 h. **Nao e defeito** — e a defesa cega descrita na secao propria. Esperado uma vez por dia. |
| `[erro] nenhum magic packet saiu. Problema de rede no ESP32.` | O `sendto()` falhou nas tres tentativas. Nao e o alvo: e a pilha de rede do proprio ESP32. Costuma vir junto de instabilidade de Wi-Fi. |

As tres abaixo sao falhas do proprio ESP32 dentro da checagem de ping.
**Todas reiniciam o aparelho**, de proposito: nenhuma delas diz nada sobre
o alvo, e devolve-las como "alvo caido" faria a sentinela mandar
Wake-on-LAN a toa num servidor que talvez esteja no ar, com o log
culpando o alvo por um defeito local. Ver `alvoResponde()`.

| Mensagem | Significado |
|---|---|
| `[erro] nao foi possivel criar a sessao de ping` | O `esp_ping_new_session()` nao conseguiu alocar: faltou heap ou socket livre. Se aparecer, conferir o heap livre dos ciclos anteriores no log. Reinicia, o que devolve memoria e sockets ao estado inicial. |
| `[erro] nao foi possivel iniciar a sessao de ping` | O `esp_ping_start()` recusou uma sessao que chegou a ser criada: faltou task, timer ou outro recurso interno. Mais raro que a anterior. Reinicia. |
| `[erro] a sessao de ping nao terminou dentro do prazo` | O callback nunca chegou: a task do ping travou ou morreu. **O mais perigoso dos tres por ser silencioso** — sem o callback, a contagem de respostas fica em zero para sempre e a sentinela seguiria "funcionando", so que cega, mandando WoL a cada ciclo. Reinicia, que e o que recupera a task. |

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

### 4. Verificar que o alvo acordou

Com o alvo desligado (mas com cabo de rede e energia conectados):

```bash
ping -c 5 192.168.X.Y
```

Se nao responder, o problema costuma ser no host, nao no ESP32:

- BIOS/UEFI: opcao "Wake on LAN" ou "Power on by PCI-E" habilitada.
- Ubuntu: NIC precisa ficar armada no shutdown. Conferir com
  `sudo ethtool <interface>` — a linha `Wake-on:` deve mostrar `g`.

### 5. Alimentacao em producao

Um carregador USB 5 V comum resolve. O dispositivo foi feito para ficar
ligado **permanentemente** — e essa a funcao dele: vigiar o alvo e
reagir sozinho. Nao precisa de intervencao depois de gravado.

Se faltar energia, ao voltar ele reinicia, pinga e retoma o ciclo. Se o
alvo estiver desligado nesse momento, ele acorda.

---

## Primeira gravacao: o que observar

Checklist para a primeira meia hora com o monitor serial aberto, antes de
deixar o aparelho sozinho na tomada. Em ordem de probabilidade real.

### 1. Firewall do alvo bloqueando ICMP  (o mais provavel)

A sentinela decide tudo pelo ping. Se o firewall do alvo descartar
ICMP echo, ela le "desligado" com o servidor ligado e passa a mandar
Wake-on-LAN para sempre numa maquina que ja esta no ar. Os pacotes sao
inofensivos, mas o diagnostico fica invertido e o log vira ruido.

**Sintoma:** `SEM RESPOSTA` a cada ciclo, mesmo com o alvo ligado e
respondendo `ping` de outra maquina.

**Conferir no alvo** (exemplo com UFW, do caso concreto do projeto)**:**

```bash
sudo ufw status verbose
ping -c 3 192.168.X.Y      # de outra maquina da rede
```

### 2. Wake-on-LAN nao armado  (o mais provavel para o wake falhar)

O magic packet pode sair perfeito e o alvo nao acordar. Isso e
configuracao do host, nao do firmware.

**Conferir no alvo, antes de desligar:**

```bash
sudo ethtool <interface> | grep Wake-on    # precisa mostrar 'g'
```

Se mostrar `d`, o WoL esta desarmado. Ativar com `sudo ethtool -s
<interface> wol g`, e tornar persistente (o ajuste nao sobrevive a
reboot sozinho). Conferir tambem a BIOS/UEFI: "Wake on LAN" ou
"Power on by PCI-E" habilitado.

### 3. MAC da interface errada

O `ALVO_MAC` precisa ser o da interface **cabeada**. Se por engano for
o do Wi-Fi, o magic packet vai para um endereco que nao existe no
segmento cabeado e nada acontece.

```bash
ip link    # no alvo, conferir qual MAC pertence a qual interface
```

### 4. Heap livre  (acompanhar, sem esperar problema)

O firmware imprime o heap livre a cada ciclo. Nao ha suspeita de
vazamento em aberto: o `esp_ping` devolve toda a memoria da sessao em ate
~1 s depois do `delete`, confirmado lendo o `ping_sock.c` do ESP-IDF, e o
firmware so volta a criar sessao 5 minutos depois.

O numero fica no log como termometro barato. Anotar o valor do primeiro
ciclo e comparar depois de umas horas: espera-se oscilacao em torno de um
patamar.

### 5. Wi-Fi

Rede 2.4 GHz e sinal suficiente no local onde o ESP32 vai ficar. O
firmware avisa explicitamente se nao conectar em 30 s.

### Ja verificado, nao precisa observar

O firmware deixa `on_ping_success` e `on_ping_timeout` como `NULL`,
definindo so `on_ping_end`. Chegou a ser levantado se o ESP-IDF invocaria
um ponteiro nulo e travaria no primeiro ping.

**Nao trava.** O fonte e publico, em
`components/lwip/apps/ping/ping_sock.c` no repositorio
[espressif/esp-idf](https://github.com/espressif/esp-idf) — na epoca a
duvida foi resolvida por outro caminho, desmontando o `ping_sock.c.obj`
do `liblwip.a` que vem no toolchain. Os tres callbacks sao carregados da
struct da sessao e cada um passa por um `beqz` que desvia da chamada
indireta quando o ponteiro e nulo:

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
   alvo esta no ar.

O ciclo de reboot nao e defeito a corrigir — e o comportamento de
recuperacao escolhido.

## Estado conhecido

- **Sem teste automatizado.** O firmware e simples o suficiente para
  validar por serial monitor.
- **Sem OTA.** Cada mudanca exige cabo USB.
- **Nada e gravado em disco.** Existe a pagina de status e existe o
  historico de 20 ocorrencias, mas os dois vivem em RAM: o reinicio os
  apaga, e o reinicio de higiene acontece a cada 24 h. A unica excecao e
  o contador de reinicios e o motivo do ultimo, que ficam em RTC RAM e
  sobrevivem ao reset por software — mas nao a queda de energia. Nao ha
  flash, cartao SD nem envio para fora: historico de semanas nao existe,
  e nao ha como reconstruir depois.
- **Nao distingue "desligado" de "inalcancavel por rede".** Se o alvo
  estiver ligado mas isolado — cabo solto, switch fora, firewall
  descartando ICMP, isolamento de clientes no Wi-Fi — a sentinela le como
  desligado e manda WoL. Sao pacotes inofensivos, mas o diagnostico fica
  invertido. Continua sendo um gap real: nao ha como fecha-lo so com
  ping.
- **Falha interna da checagem nao cai mais nesse caso.** Isto deixou de
  ser verdade para as tres falhas do proprio ESP32 dentro de
  `alvoResponde()`: elas nao saem mais como "alvo caido", reiniciam o
  aparelho com o motivo gravado. A ambiguidade que restou e so a de
  rede, do item acima.
- **Credencial em texto claro no binario.** Inerente ao Arduino. Ver a
  secao Seguranca.
- **Sem alarme.** Se o alvo nunca subir, o ESP32 tenta para sempre em
  silencio. Nao ha notificacao para fora.
- **Falha local reinicia, e o reinicio zera o estado.** As tres falhas da
  checagem de ping reiniciam o aparelho de proposito. O efeito colateral e
  que `estado` e `tentativasWol` se perdem: se o alvo estava offline
  ha varias tentativas, o contador volta a zero e as retentativas rapidas
  recomecam. Custo aceitavel — o primeiro ciclo apos o boot ja pinga e
  redescobre a situacao — mas explica um contador que reinicia sozinho no
  log.

## Verificacoes ja feitas

- Compila limpo para `esp32dev` (RAM 14,3%, Flash 60,1%), sem warnings.
- Estrutura do magic packet validada byte a byte contra a spec do
  Wake-on-LAN: 102 bytes, 6x `0xFF` + 16 repeticoes do MAC, sem lacuna
  nem estouro de buffer.
- Conversao de `IPAddress` para o `ip4_addr_t` do lwIP conferida contra
  as macros reais (`LWIP_MAKEU32` + `PP_HTONL`): os dois lados produzem
  `0xC312A8C0` para 192.168.X.Y. Um erro de ordem de bytes aqui faria
  a sentinela pingar outro host sem avisar.
- Maquina de estados simulada em quatro cenarios (ja ligado no boot;
  sobe com 1 WoL; resiste a 5 WoL; cai durante o monitoramento). As
  transicoes e os intervalos batem com o especificado.
- Broadcast para `255.255.255.255` funciona sem `setsockopt(SO_BROADCAST)`
  porque o lwIP do ESP-IDF compila com `IP_SOF_BROADCAST = 0`.
- `udp.begin()` nao e necessario: `beginPacket()` cria o socket sozinho.
- Toda sessao de ping e encerrada com `esp_ping_delete_session()`. Lendo
  o `ping_sock.c` do ESP-IDF, esse `delete` nao libera nada na hora: so
  marca a sessao para encerrar. Quem libera e a tarefa interna do ping,
  que espera com prazo de 1 segundo e, ao ver a marca, sai devolvendo a
  memoria, o buffer do pacote ICMP, o socket e a si mesma. Com 5 minutos
  ate a proxima verificacao, a margem e de 300 para 1 — nao vaza.
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
- **Duvida de vazamento na sessao de ping: encerrada.** O fonte do
  `esp_ping` e publico (`components/lwip/apps/ping/ping_sock.c`, no
  repositorio [espressif/esp-idf](https://github.com/espressif/esp-idf))
  e foi lido. O `esp_ping_delete_session()` apenas marca a sessao; quem
  devolve memoria, buffer do pacote ICMP, socket e a propria task e a
  tarefa interna do ping, em ate ~1 s. Contra 5 min ate a verificacao
  seguinte, a margem e de 300 para 1. A mesma leitura confirmou que os
  callbacks nao definidos (`on_ping_success`, `on_ping_timeout`) passam
  por checagem de ponteiro nulo antes da chamada indireta — o que antes
  so se sabia por desmontagem do `.obj` do toolchain.
- **Travas de compilacao dos oito campos do `secrets.h`**, testadas uma a
  uma com o build falhando de proposito em cada caso: template intocado,
  SSID vazio, senha vazia, nome do alvo nao editado, MAC de exemplo, IP
  do alvo de exemplo, IP do ESP32 de exemplo, gateway de exemplo, e
  mascara com numero errado de octetos. A mascara `255, 255, 255, 0` foi
  testada em separado para confirmar que ela **compila** — sem esse
  teste, a ausencia de trava de valor nela seria afirmacao nao
  verificada.
- **Caminho unico de reinicio.** Nao existe `ESP.restart()` solto no
  `main.cpp`: os cinco motivos passam por `reiniciar()`, que grava o
  texto em RTC RAM antes de reiniciar. Conferido por varredura no
  arquivo.
- **Nenhuma constante orfa.** Os 30 nomes de constante e variavel global
  do `main.cpp` foram conferidos um a um; todos tem pelo menos um uso
  alem da declaracao.
- Mensagens da serial extraidas do `main.cpp` e conferidas contra este
  README nos dois sentidos: mensagem no codigo que faltasse aqui, e
  mensagem daqui que nao existisse mais no codigo.
- **Trava do `REINICIO_PERIODICO_MS` testada por regressao.** Subindo a
  constante para 45 dias, o build para com a mensagem do `static_assert`
  explicando que a comparacao direta do `loop()` deixa de valer acima de
  ~40 dias e o que usar no lugar. Com 24 h, compila.
- **Avisos realmente ativos.** O "zero avisos sob `-Wall -Wextra`" foi
  confirmado com um teste de controle: uma variavel nao usada plantada de
  proposito no `loop()` produz `-Wunused-variable`. Sem esse controle,
  "zero avisos" poderia ser apenas flag desligada.
- `WiFi.macAddress()` funciona antes do `WiFi.mode()`: com o radio em
  `WIFI_MODE_NULL` o core le o MAC direto do efuse em vez de perguntar ao
  driver. Conferido no fonte do core, e e o que permite a linha
  `MAC do ESP32:` sair no bloco de boot, antes da rede existir.
