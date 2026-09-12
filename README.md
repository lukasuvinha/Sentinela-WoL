# Sentinela Wake-on-LAN

Dispositivo autônomo baseado em ESP32 que vigia uma máquina da rede e a
liga por Wake-on-LAN quando ela para de responder.

Fica ligado permanentemente numa tomada. A cada 5 minutos pinga o alvo;
enquanto responder, só observa. Quando para de responder, envia o magic
packet, espera o tempo de boot e confere de novo, repetindo até a máquina
subir. Serve uma página de status HTTP para consulta pelo navegador.

Não depende de servidor, serviço externo ou nuvem: a decisão inteira
acontece no próprio aparelho, dentro da rede local.

Toda a configuração vive em `src/secrets.h`, um arquivo de **oito
campos** que não vai para o git. Três deles descrevem o alvo — nome, IP e
MAC — e trocar esses três aponta a sentinela para outra máquina da mesma
rede sem alterar código; os outros cinco descrevem a rede Wi-Fi e o
endereço do próprio ESP32. Na instalação que originou o projeto, o alvo é
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

O terceiro caminho é o que separa "o alvo não respondeu" de "a
checagem não funcionou". Se a sessão de ping não pode ser criada,
não arranca, ou termina sem resposta da tarefa interna, o problema é do
ESP32 e não do alvo. Concluir "alvo desligado" nesses casos faria a
sentinela mandar Wake-on-LAN à toa num servidor que talvez esteja no ar,
e o log culparia o alvo por um defeito local. Por isso o firmware avisa e
reinicia, que é o que efetivamente cura essas três falhas. Detalhe de
cada uma na tabela de mensagens da serial, mais adiante.

O projeto roda exclusivamente em ESP32 físico. Não há simulador no
fluxo: build e teste acontecem na própria placa, via USB e serial
monitor.

### Por que ping por IP e não pelo MAC

Não dá para monitorar só pelo MAC. ICMP (ping) é camada 3 e exige um
endereço IP — não existe "pingar um MAC". O equivalente em camada 2
seria ARP, mas ARP é uma consulta indexada por IP ("quem tem
192.168.X.Y?"), então o IP continua sendo necessário.

Há um agravante: NICs armadas para Wake-on-LAN ficam parcialmente
energizadas com a máquina desligada, e algumas respondem ARP nesse
estado. ARP reportaria "ligado" para um servidor desligado — o oposto
do que se quer detectar. ICMP exige a pilha de rede do SO no ar, que é
exatamente a condição a verificar.

O IP do alvo precisa ser fixo. No caso concreto do projeto, é fixado por
netplan no próprio servidor, sem reserva de DHCP no roteador.

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
| `main.cpp` | Firmware. Lê do `secrets.h` os **oito** campos da instalação: as duas credenciais de Wi-Fi, os três do alvo (`SECRET_ALVO_NOME`, `SECRET_ALVO_MAC`, `SECRET_ALVO_IP`) e os três de endereço do próprio ESP32 (`SECRET_ESP32_IP`, `SECRET_GATEWAY_IP`, `SECRET_MASCARA_REDE`). |
| `secrets.example.h` | Template publicável. Contém os placeholders `PREENCHER_*`, nunca valores reais. Serve de referência para quem clonar o repo. |
| `secrets.h` | Dados reais: rede Wi-Fi, alvo e endereço do próprio ESP32. Está no `.gitignore`. Se não existir, criar com `cp src/secrets.example.h src/secrets.h`. |

---

## Configuração

**Tudo que é específico da instalação vive em `src/secrets.h`. O
`src/main.cpp` nunca precisa ser editado para instalar a sentinela em
outra rede ou apontar para outra máquina.**

São oito campos. Copie o template e preencha:

```bash
cp src/secrets.example.h src/secrets.h
$EDITOR src/secrets.h
```

| Campo | O que é | Onde descobrir | Trava |
|---|---|---|---|
| `SECRET_WIFI_SSID` | Nome da rede Wi-Fi. Precisa ser **2.4 GHz** — o ESP32 não fala 5 GHz. | Lista de redes do celular. Se o roteador anuncia o mesmo nome nas duas bandas, confirmar que a 2.4 está ativa. | sim |
| `SECRET_WIFI_PASSWORD` | Senha da rede. | — | sim |
| `SECRET_ALVO_NOME` | Nome da máquina vigiada. Só aparece nas mensagens da serial e na página. | Escolha sua. | sim |
| `SECRET_ALVO_MAC` | MAC da interface **cabeada** do alvo. É o endereço que o magic packet acorda. | No alvo: `ip link` (Linux) ou `ipconfig /all` (Windows). | sim |
| `SECRET_ALVO_IP` | IP do alvo. É quem o ping consulta. Precisa ser fixo. | No alvo: `ip addr`. | sim |
| `SECRET_ESP32_IP` | IP que o próprio ESP32 assume. Precisa estar **fora da faixa de DHCP** do roteador. | Painel do roteador, na configuração de DHCP. | sim |
| `SECRET_GATEWAY_IP` | Endereço do roteador. Serve de gateway e de DNS. | `ip route \| grep default` | sim |
| `SECRET_MASCARA_REDE` | Máscara da rede local. | Quase sempre `255, 255, 255, 0`. | **só quantidade** |

### Por que a máscara é a única sem trava de valor

As outras sete rejeitam o valor de exemplo em tempo de compilação: se
você copiar o template e esquecer de editar, o build falha com uma
mensagem dizendo qual campo trocar.

Com a máscara isso não funciona. `255.255.255.0` é a máscara legítima da
grande maioria das redes domésticas — rejeitar esse valor daria falso
positivo em quase toda instalação real, e obrigaria a inventar um valor
"não-exemplo" que provavelmente estaria errado. Nela só a quantidade de
octetos é conferida.

As travas de MAC e IP conferem duas coisas: a quantidade de bytes (um MAC
com cinco bytes não compila, em vez de ter o sexto preenchido com zero em
silêncio) e a diferença em relação ao valor de exemplo. Esses dois campos
ganharam trava porque são os de erro mais difícil de diagnosticar: MAC
errado não gera erro nenhum — o magic packet sai perfeito para um
endereço que não existe — e IP errado faz a sentinela concluir que o alvo
vive desligado.

### Toolchain

O `platformio.ini` fixa `platform = espressif32@7.1.1` em vez de deixar
`espressif32` solto. Sem isso, quem clonar o repo daqui a um ano pega
outra versão do toolchain e pode não compilar. Para atualizar de
propósito, subir o número e recompilar.

Não há `lib_deps`: o ping usa a API nativa do ESP-IDF (`ping_sock.h`),
que já vem no `liblwip.a` do core. O projeto não depende de nenhuma
biblioteca externa.

Também está ligado o `monitor_filters = esp32_exception_decoder`, que
traduz backtrace de crash em `arquivo:linha` em vez de endereços crus.
Útil justamente porque os três caminhos de falha da checagem reiniciam o
aparelho: se algum dia o reinício vier de um panic em vez do
`ESP.restart()`, a diferença aparece na serial.

### Tempos

Constantes no topo do `main.cpp`:

| Constante | Valor | Papel |
|---|---|---|
| `INTERVALO_MONITORAMENTO_MS` | 5&nbsp;min | Espera entre checagens com o alvo no ar. |
| `ESPERA_POS_WOL_MS` | 90&nbsp;s | Tempo dado ao alvo para subir a rede depois do WoL. No caso do projeto, um Ubuntu Server em disco mecânico. |
| `INTERVALO_RETENTATIVA_MS` | 5&nbsp;min | Espera entre WoL depois das tentativas rápidas. |
| `TENTATIVAS_RAPIDAS` | 3 | Quantos WoL a 90 s antes de espaçar para 5 min. |
| `PINGS_POR_CHECAGEM` | 3 | Basta 1 resposta para considerar online. |

O escalonamento existe para não martelar a rede quando o alvo está
fisicamente fora da tomada. Ele nunca desiste — só passa a insistir mais
devagar.

Se o alvo demorar mais de 90 s para responder ao ping depois de
ligar, o firmware manda um WoL a mais sem necessidade. Isso é inofensivo
(magic packet em máquina ligada não faz nada), mas se incomodar, aumentar
`ESPERA_POS_WOL_MS`.

### Broadcast

Fixado em `255.255.255.255` (limited broadcast). Não depende da faixa de
IP da rede local — trocar de roteador ou provedor não exige mexer aqui.

---

## Página de status

O firmware sobe um servidor HTTP na porta 80. Com o `SECRET_ESP32_IP`
preenchido como `192.168.X.Z`, a página responde em:

```
http://192.168.X.Z/
```

O endereço também aparece na serial no boot, na linha
`Pagina de status: http://`.

É preciso estar **na mesma rede local** do ESP32. Não há acesso de fora:
o aparelho não publica nada na internet, não usa nuvem e não abre túnel
nenhum.

### Somente leitura

A página não tem botão, formulário nem link de ação. Não dá para forçar
um Wake-on-LAN, reiniciar o ESP32, mudar o intervalo ou alterar
configuração alguma por ela. A única rota registrada é `/`, e tudo que
ela faz é montar HTML a partir de variáveis em memória. O que muda
comportamento está no `secrets.h` e nas constantes do `main.cpp` — ou
seja, exige recompilar e regravar pela USB.

### Não exponha na internet

**Sem redirecionamento de porta no roteador, sem UPnP, sem proxy reverso
público.**

O motivo não é a informação que a página mostra. Quem já está na LAN
descobre IP e MAC de qualquer aparelho com um `arp -a`, e a página não
revela senha nenhuma.

O motivo é outro: **um servidor web de dispositivo embarcado não é feito
para aguentar tráfego hostil.** São alguns KB de código num
microcontrolador de 320 KB de RAM, sem limite de taxa, sem timeout
agressivo, sem defesa contra requisição malformada e sem nada que
sobreviva a uma varredura automatizada — e a internet varre qualquer
porta 80 aberta em questão de horas. O risco aqui não é vazar dado: é a
sentinela parar de vigiar porque o servidor web dela travou o aparelho.

### O que ela mostra

| Bloco | Campo | O que é |
|---|---|---|
| topo | Estado e "há ..." | `ONLINE`, `OFFLINE` ou `verificando`, e há quanto tempo está assim. Conta desde a última **transição**, não desde o boot. |
| Verificação | Última | Há quanto tempo foi o último ping. |
| Verificação | Próxima&nbsp;em | Quanto falta para a próxima. Mostra `agora` quando o ciclo já deveria ter acontecido — o aparelho está no meio de uma checagem. |
| Verificação | WoL desde a última subida | Magic packets enviados desde a última vez que o alvo respondeu. Zera quando ele sobe. |
| Verificação | WoL desde o boot | Total acumulado desde que o ESP32 ligou. Não zera. |
| ESP32 | Firmware | A versão editada a mão e o momento da compilação. Ver "Carimbo de versão". |
| ESP32 | Ligado&nbsp;há | Tempo desde o último boot. Ver a ressalva do reinício de 24 h, adiante. |
| ESP32 | Heap&nbsp;livre | Memória livre **neste instante**. |
| ESP32 | Mínimo desde o boot | **O pior momento de memória livre, não o valor atual.** Ver abaixo. |
| ESP32 | IP&nbsp;/&nbsp;MAC | Do próprio ESP32, lidos da pilha de rede — o que ele de fato está usando. |
| ESP32 | Reinícios | Quantos desde a última queda de energia, e o motivo do último. |
| Alvo | IP&nbsp;/&nbsp;MAC | Os valores compilados, vindos do `secrets.h`. Serve para conferir na hora se o firmware gravado é o que se pensa que é. |
| Ocorrências | lista | Até 20 eventos, do mais recente para o mais antigo. |

**"Mínimo desde o boot" merece atenção** porque é fácil ler errado. Não é
a leitura atual: é o menor valor que o heap livre já atingiu desde que o
aparelho ligou (`ESP.getMinFreeHeap()`). Ele só desce, nunca sobe. É ele
que responde a pergunta que importa — "em algum momento chegou perto do
fim?". O "Heap livre" da linha de cima pode estar confortável agora e ter
havido um aperto há seis horas; só o mínimo mostra isso.

### Atualização automática

A página se recarrega sozinha a cada 10 segundos, por
`<meta http-equiv="refresh">`. **Não há JavaScript e não há recurso
externo** — nenhuma fonte, folha de estilo ou biblioteca vinda de CDN.

Isso é deliberado: a página precisa abrir com a internet fora, que é
exatamente quando alguém vai querer olhar o estado do servidor de casa.
Uma página que dependesse de `<script src="https://...">` ficaria em
branco justamente na hora útil.

O HTML também é enviado em blocos (chunked), montado num buffer fixo de
320 bytes, nunca numa `String`. Com refresh de 10 s são ~8.600
requisições por dia; montar a página inteira em heap a cada uma daria
dezenas de milhares de alocações diárias no aparelho que o resto do
projeto trabalha para manter sem fragmentação.

---

## IP fixo do ESP32

O aparelho assume um endereço fixo em vez de pedir um por DHCP. Sem isso
o IP muda sem aviso e a página de status "some" justamente quando alguém
precisa dela.

Três campos do `secrets.h` definem isso:

| Campo | Exemplo | Papel |
|---|---|---|
| `SECRET_ESP32_IP` | `192, 168, X, Z` | O endereço que o ESP32 assume. |
| `SECRET_GATEWAY_IP` | `192, 168, X, 1` | O roteador. Serve de gateway e também de DNS. |
| `SECRET_MASCARA_REDE` | `255, 255, 255, 0` | Máscara da rede local. |

A configuração é aplicada dentro de `garantirWiFi()`, e não só no
`setup()`. Isso é de propósito: assim vale também em toda reconexão, e o
endereço não depende da ordem das chamadas na inicialização.

### A armadilha: a faixa de DHCP

**O IP escolhido precisa estar FORA da faixa que o roteador distribui por
DHCP.**

Se estiver dentro dela, o roteador não tem como saber que aquele endereço
está ocupado — o ESP32 nunca pediu nada a ele — e mais cedo ou mais tarde
entrega o mesmo endereço a outro aparelho. Os dois passam a responder
pelo mesmo IP e somem da rede **de forma intermitente**: às vezes
funciona, às vezes não, dependendo de quem respondeu ARP por último. É
dos sintomas mais chatos de diagnosticar, porque não aparece erro em
lugar nenhum — nem no ESP32, nem no roteador, nem no outro aparelho.

Roteador doméstico típico distribui de `.100` a `.200`, e aí qualquer
coisa abaixo de `.100` serve. Alguns distribuem de `.2` a `.254`: nesse
caso não sobra faixa livre, e é preciso encurtar o intervalo do DHCP no
painel antes de fixar qualquer endereço.

**Como conferir o endereço do roteador:**

```bash
ip route | grep default        # o IP do roteador vem depois de "via"
```

**Como conferir a faixa de DHCP:** só no painel do roteador. Abrir o IP
do gateway no navegador e procurar por "DHCP", "Servidor DHCP" ou "LAN
Settings". O nome do campo varia entre fabricantes: "faixa de
endereços", "pool", "start/end address".

### Voltar para DHCP

Comentar uma única linha no `main.cpp`, dentro do bloco delimitado por
`IP FIXO - inicio do bloco` / `fim do bloco`, em `garantirWiFi()`:

```cpp
// WiFi.config(ESP32_IP, GATEWAY_IP, MASCARA_REDE, GATEWAY_IP);
```

O aparelho passa a pegar endereço do roteador. O IP obtido aparece na
serial em duas linhas — `Wi-Fi OK. IP do ESP32:` e
`Pagina de status: http://` — que nesse modo continuam corretas, porque
as duas saem de `WiFi.localIP()` depois de a conexão existir, e não da
constante. A página de status continua funcionando, só que num endereço
que pode mudar sem aviso.

### Reserva de DHCP, a alternativa

Quem preferir centralizar o controle no roteador pode deixar o ESP32 em
DHCP e criar uma **reserva** lá, amarrando o endereço ao MAC do aparelho.
Para isso o firmware imprime o próprio MAC no boot:

```
MAC do ESP32: 3C:61:05:XX:XX:XX
```

Serve também para identificar o aparelho na lista de clientes do
roteador, onde sem isso ele fica como mais um dispositivo sem nome no
meio dos outros.

---

## Reinício periódico de higiene

A cada 24 horas de funcionamento o aparelho se reinicia sozinho.

A checagem está no início de cada ciclo do `loop()`, não num temporizador
independente. Na prática o reinício cai no **primeiro ciclo depois das
24 h** — algo entre 24h00 e 24h05, porque com o alvo no ar cada ciclo
dura 5 minutos. Não é um horário exato, e não precisa ser.

A serial anuncia:

```
Reinicio periodico de higiene (24h de funcionamento).
```

### É defesa cega, não diagnóstico

Isto não corrige um problema conhecido. Existe contra degradação que o
código **não tem como perceber sozinho**.

Dois casos motivam. O primeiro é fragmentação de heap: o total livre pode
continuar alto enquanto já não existe nenhum bloco contíguo grande o
suficiente, e não há como checar isso de dentro com confiança.

O segundo é mais grave — a pilha de Wi-Fi entrar num estado em que
reporta `WL_CONNECTED` sem tráfego real passar. Nesse estado nada falha
visivelmente: `garantirWiFi()` vê a conexão como boa e não reconecta, o
ping falha honestamente porque não há rede, a sentinela conclui "alvo
desligado" e passa a mandar Wake-on-LAN para sempre num servidor que
está ligado o tempo todo. Nenhuma das três mensagens de erro da checagem
aparece, porque nenhuma delas descreve esse caso. O aparelho continua
"funcionando", só que cego — e não há como distinguir isso de um alvo
realmente fora do ar sem sair do próprio ESP32.

### Não há condição de estado, de propósito

O reinício acontece mesmo com o alvo offline no meio das tentativas de
Wake-on-LAN. Isso foi decidido, não esquecido.

Condicionar o reinício a "só quando estiver tudo calmo" criaria
exatamente o caminho que a defesa existe para cobrir: um aparelho
degradado que nunca se recupera justamente por estar ocupado. No cenário
do Wi-Fi fantasma acima, a sentinela ficaria permanentemente em OFFLINE
mandando WoL — portanto nunca "calma" — e nunca reiniciaria.

O custo de reiniciar no meio de uma sequência é pequeno: depois do boot o
primeiro ciclo pinga em ~30 s, contra um intervalo de retentativa de
5 min.

### O custo

Cerca de **30 segundos de cegueira por dia** — boot, conexão de Wi-Fi e
primeiro ping. São 0,03% do tempo. Se o alvo cair exatamente nessa
janela, a detecção atrasa um ciclo.

### O efeito colateral

**O tempo de funcionamento e o histórico na página nunca passam de
24 horas.** Não há defeito nisso: o "Ligado há" mostra o tempo desde o
último reinício, e as ocorrências vivem em RAM comum, que o reinício
apaga. Quem abrir a página esperando um histórico de semanas vai achar
que algo se perdeu — não se perdeu, nunca esteve lá.

O contador de reinícios e o motivo do último são a exceção: sobrevivem,
pelo mecanismo da próxima seção.

---

## Histórico de ocorrências e contadores de reinício

A página de status mostra até **20 ocorrências**, da mais recente para a
mais antiga, com "há quanto tempo" ao lado de cada uma.

### Vetor de tamanho fixo, e por que

O histórico é um vetor circular de 20 posições, com texto de 56 bytes
cada, reservado uma vez na inicialização e imutável dali em diante. Sem
`String`, sem `new`, sem `malloc`, sem `std::vector`. Quando a 21a
ocorrência chega, ela sobrescreve a mais antiga.

A razão é a mesma que guia o resto do firmware: este aparelho fica meses
ligado registrando eventos. Texto de tamanho dinâmico neste caminho
significaria alocar e liberar blocos de tamanhos variados milhares de
vezes, que é a receita de fragmentação de heap — exatamente o problema
que o projeto passou uma revisão inteira descartando. Um log bonito não
paga reintroduzi-lo pela porta dos fundos.

O preço do tamanho fixo é o truncamento: uma mensagem que passe de 56
bytes é cortada em silêncio pelo `snprintf`. Com nome de máquina curto
isso não acontece.

### O critério: só ocorrências, nunca rotina

Entra no histórico o que é **evento**: boot, conexão de Wi-Fi, queda e
volta do Wi-Fi, mudança de estado do alvo, envio de Wake-on-LAN, falha de
envio, e o motivo de um reinício anterior. Não entra o tique de rotina —
o `continua online` de cada ciclo.

A primeira conexão e a reconexão são registradas com textos diferentes
(`Conectando no Wi-Fi` e `Wi-Fi caiu - reconectando`). A distinção
importa: até a versão anterior o boot gravava uma queda de Wi-Fi que
nunca tinha acontecido, e como o boot só era registrado depois, um
aparelho recém-ligado exibia os eventos em ordem enganosa — o boot
aparecia como o mais recente dos três, acima da conexão que tinha
partido dele.

Isso é essencial, não economia de memória. Com o alvo no ar, um ciclo
acontece a cada 5 minutos. Se cada um gravasse uma linha, as 20 posições
se esgotariam em **100 minutos**, e qualquer ocorrência real seria
empurrada para fora antes de alguém ter chance de ver. O histórico existe
para responder "o que aconteceu de diferente", e uma lista de vinte
`continua online` não responde nada.

### As categorias de ocorrência

O firmware registra seis categorias de evento. A lista abaixo é **das
categorias**, não das mensagens: o texto exato de cada ocorrência não é
transcrito aqui de propósito.

| Categoria | Quando entra |
|---|---|
| Boot | Uma vez por inicialização, antes de tudo |
| Transição de Wi-Fi | Primeira conexão, queda e reconexão — os três com textos distintos |
| Mudança de estado do alvo | Passou a responder, ou parou de responder |
| Wake-on-LAN enviado | Uma por tentativa, com o número da tentativa e quantos pacotes saíram |
| Erro | Falha que o firmware detectou e não conseguiu resolver sozinho |
| Motivo do último reinício | Reinjetado no boot, a partir da RTC RAM |

O motivo de não transcrever as mensagens é que elas têm natureza
diferente das da serial. As mensagens de serial são material de
diagnóstico: quem lê o log precisa saber o que cada uma significa, e por
isso elas aparecem em tabela mais adiante. As ocorrências do histórico
são lidas **dentro da página**, ao lado do estado atual e do "há quanto
tempo" — o contexto que elas precisam já está na tela. Copiá-las para cá
produziria uma segunda lista para manter em sincronia, e a lista que
diverge primeiro é sempre a que ninguém lê.

Quem for conferir documentação contra código neste canal deve comparar
**por categoria**: uma ocorrência nova só é divergência se não couber em
nenhuma das seis acima.

### Contadores que sobrevivem ao reinício

O histórico vive em RAM comum, e o reinício o apaga. Isso deixava um
buraco justamente no que mais importa: os eventos que **disparam** um
reinício eram os únicos que nunca chegavam a aparecer na página, porque o
próprio reinício que eles anunciavam os destruía.

Duas coisas foram para a **RTC RAM**, uma região pequena que o reset por
software não zera:

- quantos reinícios já houve;
- o motivo do último, em texto.

No boot o firmware lê as duas, imprime na serial e **reinjeta o motivo no
histórico**, como uma ocorrência `Reiniciou: <motivo>`. É por isso que a
página consegue dizer POR QUE o aparelho reiniciou, mesmo o reinício
tendo apagado a memória onde essa informação estava.

```
Reinicios desde a ultima queda de energia: 3
Motivo do ultimo: reinicio periodico de higiene (24h)
```

**A RTC RAM não sobrevive a queda de energia, e essa é a semântica
desejada.** Falta de luz não é sintoma de defeito do aparelho: se o
contador continuasse somando entre apagões, passaria a medir a
confiabilidade da rede elétrica em vez da saúde do firmware. Tirar o
aparelho da tomada zera a contagem de propósito — daí o "desde a última
queda de energia" no texto.

Uma palavra mágica gravada junto distingue dado nosso de lixo: depois de
um power-on a região vem com qualquer conteúdo, e sem essa checagem o
firmware anunciaria um número aleatório de reinícios com um motivo
ilegível.

Os motivos possíveis hoje são cinco:

| Motivo | Origem |
|---|---|
| `Wi-Fi nao conectou dentro do prazo` | 30 s sem conectar, em `garantirWiFi()`. |
| `falha ao criar a sessao de ping` | `esp_ping_new_session()` recusou. |
| `falha ao iniciar a sessao de ping` | `esp_ping_start()` recusou. |
| `ping sem retorno dentro do prazo` | O callback do ping nunca veio. |
| `reinicio periodico de higiene (24h)` | O reinício programado. Único sem defeito envolvido. |

Todo reinício passa por uma única função, `reiniciar()`, que grava o
motivo antes de chamar `ESP.restart()`. Não existe `ESP.restart()` solto
no arquivo — logo, não existe reinício sem motivo registrado.

---

## Carimbo de versão

O firmware se identifica em dois lugares, com a mesma informação: na
serial, logo abaixo do cabeçalho de boot, e na página de status, na
primeira linha do bloco ESP32.

```
Firmware 1.0, compilado em Sep 11 2026 16:45:12
```

São dois dados de natureza diferente:

| Parte | De onde vem | O que significa |
|---|---|---|
| `1.0` | `FIRMWARE_VERSAO`, no `main.cpp` | **Editada a mão** a cada versão significativa. Diz que versão se quis gravar. |
| `Sep 11 2026 16:45:12` | `__DATE__` e `__TIME__` | Substituídos pelo pré-processador. Diz quando este binário foi feito. |

Não há nada automático atrás do número de versão — nem tag de git, nem
contador de build — e isso é deliberado. O aparelho é gravado por USB, de
um clone que pode estar em qualquer ponto do histórico, às vezes com
alteração não commitada; um número gerado automaticamente daria
impressão de rastreabilidade que não existe. O número diz a intenção, e o
carimbo de compilação é o dado objetivo ao lado dele.

**A ressalva do `__DATE__`/`__TIME__`.** Os dois congelam no momento em
que o **`main.cpp`** é compilado, e só então. Numa compilação incremental
que não recompile esse arquivo — mexeu só no `secrets.h`, por exemplo — o
carimbo antigo vai inteiro para dentro do binário novo, e a placa passa a
mentir sobre a própria idade justamente quando se está tentando descobrir
qual firmware ela tem.

Por isso o build de validação do projeto é **sempre do zero**:

```bash
rm -rf .pio/build && pio run
```

Para que serve na prática: com o aparelho já na tomada há meses, abrir a
página e comparar o carimbo com o do último build responde em dois
segundos a pergunta "esta placa tem a versão que eu acho que tem?" — sem
desmontar nada e sem cabo USB.

---

## Segurança

O firmware embute o SSID e a senha **em texto claro** dentro do binário.
Isso não é um defeito deste projeto — é como qualquer sketch Arduino
funciona. Dá para conferir:

```bash
strings .pio/build/esp32dev/firmware.bin | grep -i <seu-ssid>
```

Consequência prática: **nunca** publicar `.pio/`, nem anexar um
`firmware.bin` compilado em issue, gist ou release. O `.gitignore`
cobre `.pio/`, `*.bin`, `*.elf` e `*.map` justamente por isso.

Demais regras:

- O `.gitignore` bloqueia **qualquer** nome contendo `secrets` ou
  `credentials`, em qualquer diretório e em qualquer combinação de
  maiúsculas, mais backups (`*.bak`, `*.old`, `*.orig`, `*.save`, `*~`),
  `.env` e chaves (`*.pem`, `*.key`). A única exceção é
  `secrets.example.h`, que não tem valores reais.

  As regras são amplas porque duas versões mais estreitas já falharam em
  teste: `src/secrets.h` deixava passar `secrets.h.bak`, e `*secrets*`
  deixava passar `SECRETS.h` — o gitignore diferencia maiúsculas. Daí as
  classes de caractere (`*[Ss][Ee][Cc]...`) no arquivo: são feias, mas
  cobrem as 128 combinações de caixa.

- O `.gitignore` só protege arquivo **não rastreado**. Se algo já foi
  commitado, continua no histórico mesmo depois de entrar na lista.
- O template `secrets.example.h` nunca recebe valor real. Ele carrega os
  placeholders `PREENCHER_*`, e não string vazia: o `static_assert` rejeita
  os dois, mas até então o template usava `""` e a compilação passava,
  gerando firmware com credenciais em branco.
- Antes do primeiro push, conferir o que realmente seria publicado:
  ```bash
  git add -A && git status --porcelain
  ```
  Devem aparecer apenas: `.gitignore`, `README.md`, `platformio.ini`,
  `src/main.cpp`, `src/secrets.example.h` e `.vscode/extensions.json`.
- Se `secrets.h` for commitado por acidente, tratar a senha como vazada:
  **trocar a senha do Wi-Fi**. Remover o arquivo num commit seguinte não
  resolve — o valor continua no histórico do git.
- Quem tem o firmware tem a senha da rede. Se o ESP32 for emprestado,
  descartado ou vendido, apagar a flash (`esptool.py erase_flash`).
- O firmware chama `WiFi.persistent(false)`. O padrão do core é `true`, o
  que guardaria a senha também na NVS — uma segunda cópia, sem utilidade
  aqui, já que as credenciais vêm compiladas via `secrets.h`.

  **A chamada precisa vir antes de `WiFi.mode()`**, e isso não é óbvio:
  `persistent()` só grava uma flag interna. Quem age sobre ela é
  `wifiLowLevelInit()`, que roda uma única vez (protegida por
  `lowLevelInitDone`) e é disparada justamente pelo `WiFi.mode()`. Chamar
  `persistent(false)` depois do `mode()` não tem efeito nenhum — o
  storage já ficou em NVS e não há segunda chance. Quem reordenar o
  `setup()` desfaz a proteção sem nenhum aviso do compilador.

---

## Como gravar no ESP32

### 1. Preparar o firmware

```bash
cd /caminho/para/sentinela-wol
cp src/secrets.example.h src/secrets.h   # se ainda nao existir
$EDITOR src/secrets.h                     # preencher os OITO campos
```

São oito campos, não dois: as duas credenciais de Wi-Fi, os três do alvo
e os três de endereço do próprio ESP32. A seção
[Configuração](#configuração) traz cada um em tabela, com o que é e onde
descobrir o valor.

Preencher só o SSID e a senha não gera um firmware pela metade — o build
para, com uma mensagem por campo que faltou. A lista completa está em
[Erro de compilação esperado](#erro-de-compilação-esperado).

### 2. Conectar a placa

Cabo USB **de dados** (não só de carga) entre ESP32 e notebook.
Verificar se o Linux reconheceu:

```bash
ls /dev/ttyUSB* /dev/ttyACM*   # tipicamente /dev/ttyUSB0
dmesg | tail                    # em caso de duvida
```

Se aparecer mas der permissão negada no upload:

```bash
sudo usermod -aG dialout $USER
# fazer logout/login para o grupo entrar em vigor
```

### 3. Build e upload

**Pela extensão PlatformIO no VSCodium** — barra lateral, ícone do
PlatformIO:

1. **Project Tasks → esp32dev → General → Build**
2. Se compilar sem erro: **Project Tasks → esp32dev → General → Upload**
3. **Project Tasks → esp32dev → General → Monitor** (serial 115200 baud)

**Pela linha de comando** — equivalente, útil para script:

```bash
cd /caminho/para/sentinela-wol
pio run                 # compila
pio run -t upload       # compila e grava na placa
pio device monitor      # abre a serial a 115200
```

Se a placa não for detectada sozinha, informar a porta:

```bash
pio run -t upload --upload-port /dev/ttyUSB0
```

Algumas placas exigem segurar o botão **BOOT** durante o início do
upload (quando aparece `Connecting....`), soltando depois.

Referência de tamanho de um build limpo (esp32dev, 4 MB flash):
RAM 14,3% (46.776 B), Flash 60,1% (787.193 B).

Nos exemplos abaixo o `SECRET_ALVO_NOME` está preenchido com
`servidor`, e MAC e IP aparecem como placeholders.

Com o alvo já ligado, a serial mostra:

```
=== Sentinela Wake-on-LAN ===
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

- **`Conectando no Wi-Fi...` é diferente de `Wi-Fi desconectado.
  Reconectando...`.** A mesma funcao (`garantirWiFi()`) trata a primeira
  conexão e as reconexões, mas anuncia cada caso com o texto que
  corresponde. No boot só aparece o primeiro; o segundo significa que uma
  conexão que existia caiu. Os pontos que vêm depois são o progresso da
  tentativa.
- **`Pagina de status:` só sai depois de `Wi-Fi OK`**, e imprime o
  endereço que a interface de fato assumiu (`WiFi.localIP()`). Vale nos
  dois modos, IP fixo ou DHCP.

Depois de um reinício, é só depois dele, aparecem mais duas linhas logo
abaixo do carimbo de versão:

```
=== Sentinela Wake-on-LAN ===
Firmware 1.0, compilado em Sep 11 2026 16:45:12
Reinicios desde a ultima queda de energia: 3
Motivo do ultimo: reinicio periodico de higiene (24h)
Alvo: servidor  192.168.X.Y  AA:BB:CC:DD:EE:FF
```

Elas somem quando o aparelho é desligado da tomada — a contagem vive em
RTC RAM, que a queda de energia zera. Ver a seção de histórico e
contadores.

O **heap livre** aparece em todo ciclo como termômetro barato, não
porque haja suspeita em aberto.

O aparelho cria e destrói uma sessão de ping a cada 5 min, para sempre
(~105 mil por ano), o que já levantou a dúvida de vazamento. Ela está
encerrada: lendo o `ping_sock.c` do ESP-IDF, o `esp_ping_delete_session()`
não libera nada na hora — só marca a sessão para encerrar. Quem libera é
a tarefa interna do ping, que espera com prazo de 1 segundo e, ao ver a
marca, sai devolvendo a memória, o buffer do pacote ICMP, o socket e a si
mesma. Como o firmware espera 5 minutos até a próxima verificação, a
margem é de 300 para 1.

A partir daí repete `continua online` a cada 5 minutos. Com o alvo
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
instrumentação de memória: o bloco de cima (alvo ligado) foi
corrigido na época, este aqui passou despercebido e só apareceu numa
verificação posterior que comparou mensagem por mensagem.

Não foi apagado porque documenta que o formato do log mudou - quem
encontrar uma gravação de serial antiga, sem o heap, sabe que é de uma
versão anterior e não um defeito.

Verificando o servidor... SEM RESPOSTA.
Enviando Wake-on-LAN (tentativa 1) para AA:BB:CC:DD:EE:FF
  3/3 pacotes enviados.
Aguardando 90s antes de verificar de novo.
Verificando o servidor... ONLINE.
Subiu depois de 1 tentativa(s) de Wake-on-LAN.
-->


Se o Wi-Fi não conectar em 30 segundos, o firmware desiste, explica o
motivo provável e reinicia sozinho para tentar de novo:

```
FALHA: nao conectou no Wi-Fi dentro do timeout.
Confira SSID/senha em src/secrets.h.
Confira tambem se a rede e 2.4 GHz (o ESP32 nao fala 5 GHz).
Reiniciando em 10s para tentar de novo...
```

### Outras mensagens da serial

| Mensagem | Significado |
|---|---|
| `Firmware <versao>, compilado em <data> <hora>` | Só no boot. Ver a seção "Carimbo de versão". |
| `Conectando no Wi-Fi...` | Primeira conexão do ciclo de vida atual — boot ou reinício. Nada caiu. |
| `Wi-Fi desconectado. Reconectando...` | Uma conexão que **existia** caiu. Normal é transitório: toda rede cai de vez em quando, e o firmware reconecta sozinho. Preocupante só se aparecer a cada ciclo. |
| `Wi-Fi OK. IP do ESP32: <ip>` | Conexão estabelecida. Com o IP fixo ativo, este endereço é o do `SECRET_ESP32_IP`; se divergir, o `WiFi.config()` não está valendo. |
| `MAC do ESP32: <mac>` | Só no boot. O MAC do próprio aparelho, para reserva de DHCP no roteador e para identificá-lo na lista de clientes. |
| `Pagina de status: http://<ip>` | Só no boot, **depois** de a rede existir. O endereço vem de `WiFi.localIP()`, então é o real nos dois modos. |
| `Reinicios desde a ultima queda de energia: <n>` | Só no boot, e só se houve reinício. Vem da RTC RAM; zera quando falta energia. |
| `Motivo do ultimo: <texto>` | Acompanha a linha acima. Um dos cinco motivos da tabela da seção de histórico. |
| `Reinicio periodico de higiene (24h de funcionamento).` | O reinício programado de 24 h. **Não é defeito** — é a defesa cega descrita na seção própria. Esperado uma vez por dia. |
| `[erro] nenhum magic packet saiu. Problema de rede no ESP32.` | O `sendto()` falhou nas três tentativas. Não é o alvo: é a pilha de rede do próprio ESP32. Costuma vir junto de instabilidade de Wi-Fi. |

As três abaixo são falhas do próprio ESP32 dentro da checagem de ping.
**Todas reiniciam o aparelho**, de propósito: nenhuma delas diz nada sobre
o alvo, e devolvê-las como "alvo caído" faria a sentinela mandar
Wake-on-LAN à toa num servidor que talvez esteja no ar, com o log
culpando o alvo por um defeito local. Ver `alvoResponde()`.

| Mensagem | Significado |
|---|---|
| `[erro] nao foi possivel criar a sessao de ping` | O `esp_ping_new_session()` não conseguiu alocar: faltou heap ou socket livre. Se aparecer, conferir o heap livre dos ciclos anteriores no log. Reinicia, o que devolve memória e sockets ao estado inicial. |
| `[erro] nao foi possivel iniciar a sessao de ping` | O `esp_ping_start()` recusou uma sessão que chegou a ser criada: faltou task, timer ou outro recurso interno. Mais raro que a anterior. Reinicia. |
| `[erro] a sessao de ping nao terminou dentro do prazo` | O callback nunca chegou: a task do ping travou ou morreu. **O mais perigoso dos três por ser silencioso** — sem o callback, a contagem de respostas fica em zero para sempre e a sentinela seguiria "funcionando", só que cega, mandando WoL a cada ciclo. Reinicia, que é o que recupera a task. |

### Erro de compilação esperado

Se algum campo do `secrets.h` não estiver utilizável, o build **falha de
propósito**, antes de gerar firmware. São **quinze travas**, duas por
campo de texto e duas por campo numérico — uma pega o descuido de não
editar, outra o de editar errado. A máscara é a única com uma só, pelo
motivo explicado na seção de configuração.

As mensagens saem sem acento porque vêm do compilador, e são exatamente
estas:

| Situação | Mensagem |
|---|---|
| Copiou o template e não editou o SSID | `Preencha SECRET_WIFI_SSID em src/secrets.h antes de compilar.` |
| Apagou o SSID e deixou `""` | `SECRET_WIFI_SSID esta vazio em src/secrets.h. Nao existe rede sem nome.` |
| Idem, campo da senha | `Preencha SECRET_WIFI_PASSWORD em src/secrets.h antes de compilar.` |
| Apagou a senha e deixou `""` | `SECRET_WIFI_PASSWORD esta vazia em src/secrets.h. Veja o comentario acima se a rede for aberta.` |
| Idem, nome do alvo | `Preencha SECRET_ALVO_NOME em src/secrets.h antes de compilar.` |
| Apagou o nome do alvo e deixou `""` | `SECRET_ALVO_NOME esta vazio em src/secrets.h.` |
| MAC do alvo com número de bytes errado — cinco, sete | `SECRET_ALVO_MAC precisa ter exatamente 6 bytes em src/secrets.h.` |
| MAC do alvo ainda no valor do template | `SECRET_ALVO_MAC ainda e o exemplo. Troque em src/secrets.h pelo MAC real do alvo.` |
| IP do alvo com número de octetos errado | `SECRET_ALVO_IP precisa ter exatamente 4 octetos em src/secrets.h.` |
| IP do alvo ainda no valor do template | `SECRET_ALVO_IP ainda e o exemplo. Troque em src/secrets.h pelo IP real do alvo.` |
| IP do ESP32 com número de octetos errado | `SECRET_ESP32_IP precisa ter exatamente 4 octetos em src/secrets.h.` |
| IP do ESP32 ainda no valor do template | `SECRET_ESP32_IP ainda e o exemplo. Escolha em src/secrets.h um IP livre, fora da faixa de DHCP do roteador.` |
| Gateway com número de octetos errado | `SECRET_GATEWAY_IP precisa ter exatamente 4 octetos em src/secrets.h.` |
| Gateway ainda no valor do template | `SECRET_GATEWAY_IP ainda e o exemplo. Troque em src/secrets.h pelo IP do seu roteador.` |
| Máscara com número de octetos errado | `SECRET_MASCARA_REDE precisa ter exatamente 4 octetos em src/secrets.h.` |

Quem copia o template e não edita nada recebe sete delas de uma vez: as
três de `Preencha` mais as quatro de `ainda e o exemplo`. As de
quantidade e as de campo vazio não disparam nesse caso, porque o template
tem a contagem certa de bytes e nenhum campo em branco.

Isso é intencional: evita gravar uma placa com credencial inválida e só
descobrir depois, olhando a serial — onde o sintoma seria um ciclo de
reinício sem explicação.

A checagem de string vazia foi acrescentada depois de uma auditoria
mostrar que o caminho ensinado por este mesmo README
(`cp src/secrets.example.h src/secrets.h`) compilava sem reclamar: o
template usava `""`, e a única checagem existente comparava contra
`PREENCHER_*`. String vazia não é igual ao placeholder, então o build
passava e produzia firmware com SSID e senha em branco.

**Rede aberta.** Senha vazia é legítima apenas nesse caso. Para permitir,
comentar o `static_assert` correspondente no `main.cpp` — há um
comentário no lugar explicando. Vale lembrar que rede aberta é escolha
ruim para um aparelho que fica ligado permanentemente.

#### A décima sexta trava, de natureza diferente

Existe mais um `static_assert` no `main.cpp`, e ele **não** tem relação
com o `secrets.h`. Não é campo que o usuário preenche: é um teto de
projeto sobre `REINICIO_PERIODICO_MS`, a constante do reinício de
higiene. Só aparece para quem alterar aquele valor no código — quem
apenas instala a sentinela nunca vai vê-lo.

```
REINICIO_PERIODICO_MS perto demais do estouro de millis() (~49,7 dias). Acima de ~40 dias a comparacao direta do loop() deixa de valer: troque por (millis() - referencia) > REINICIO_PERIODICO_MS, guardando a referencia do boot numa variavel.
```

O motivo está na seção do reinício periódico: a comparação do `loop()` é
direta (`millis() > REINICIO_PERIODICO_MS`) e só vale porque a referência
é o boot e 24 h está longe do estouro de ~49,7 dias. O teto de 40 dias
existe para que aumentar a constante além disso pare o build, em vez de
produzir um aparelho que reinicia na hora errada meses depois.

### 4. Verificar que o alvo acordou

Com o alvo desligado (mas com cabo de rede e energia conectados):

```bash
ping -c 5 192.168.X.Y
```

Se não responder, o problema costuma ser no host, não no ESP32:

- BIOS/UEFI: opção "Wake on LAN" ou "Power on by PCI-E" habilitada.
- Ubuntu: NIC precisa ficar armada no shutdown. Conferir com
  `sudo ethtool <interface>` — a linha `Wake-on:` deve mostrar `g`.

### 5. Alimentação em produção

Um carregador USB 5 V comum resolve. O dispositivo foi feito para ficar
ligado **permanentemente** — é essa a função dele: vigiar o alvo e
reagir sozinho. Não precisa de intervenção depois de gravado.

Se faltar energia, ao voltar ele reinicia, pinga e retoma o ciclo. Se o
alvo estiver desligado nesse momento, ele acorda.

---

## Primeira gravação: o que observar

Checklist para a primeira meia hora com o monitor serial aberto, antes de
deixar o aparelho sozinho na tomada. Em ordem de probabilidade real.

### 1. Firewall do alvo bloqueando ICMP  (o mais provável)

A sentinela decide tudo pelo ping. Se o firewall do alvo descartar
ICMP echo, ela lê "desligado" com o servidor ligado e passa a mandar
Wake-on-LAN para sempre numa máquina que já está no ar. Os pacotes são
inofensivos, mas o diagnóstico fica invertido e o log vira ruído.

**Sintoma:** `SEM RESPOSTA` a cada ciclo, mesmo com o alvo ligado e
respondendo `ping` de outra máquina.

**Conferir no alvo** (exemplo com UFW, do caso concreto do projeto)**:**

```bash
sudo ufw status verbose
ping -c 3 192.168.X.Y      # de outra maquina da rede
```

### 2. Wake-on-LAN não armado  (o mais provável para o wake falhar)

O magic packet pode sair perfeito e o alvo não acordar. Isso é
configuração do host, não do firmware.

**Conferir no alvo, antes de desligar:**

```bash
sudo ethtool <interface> | grep Wake-on    # precisa mostrar 'g'
```

Se mostrar `d`, o WoL está desarmado. Ativar com `sudo ethtool -s
<interface> wol g`, e tornar persistente (o ajuste não sobrevive a
reboot sozinho). Conferir também a BIOS/UEFI: "Wake on LAN" ou
"Power on by PCI-E" habilitado.

### 3. MAC da interface errada

O `ALVO_MAC` precisa ser o da interface **cabeada**. Se por engano for
o do Wi-Fi, o magic packet vai para um endereço que não existe no
segmento cabeado e nada acontece.

```bash
ip link    # no alvo, conferir qual MAC pertence a qual interface
```

### 4. Heap livre  (acompanhar, sem esperar problema)

O firmware imprime o heap livre a cada ciclo. Não há suspeita de
vazamento em aberto: o `esp_ping` devolve toda a memória da sessão em até
~1 s depois do `delete`, confirmado lendo o `ping_sock.c` do ESP-IDF, e o
firmware só volta a criar sessão 5 minutos depois.

O número fica no log como termômetro barato. Anotar o valor do primeiro
ciclo e comparar depois de umas horas: espera-se oscilação em torno de um
patamar.

### 5. Wi-Fi

Rede 2.4 GHz e sinal suficiente no local onde o ESP32 vai ficar. O
firmware avisa explicitamente se não conectar em 30 s.

### Já verificado, não precisa observar

O firmware deixa `on_ping_success` e `on_ping_timeout` como `NULL`,
definindo só `on_ping_end`. Chegou a ser levantado se o ESP-IDF invocaria
um ponteiro nulo e travaria no primeiro ping.

**Não trava.** O fonte é público, em
`components/lwip/apps/ping/ping_sock.c` no repositório
[espressif/esp-idf](https://github.com/espressif/esp-idf) — na época a
dúvida foi resolvida por outro caminho, desmontando o `ping_sock.c.obj`
do `liblwip.a` que vem no toolchain. Os três callbacks são carregados da
struct da sessão e cada um passa por um `beqz` que desvia da chamada
indireta quando o ponteiro é nulo:

```
l32i   a4, a2, 112    ; carrega o ponteiro do callback
beqz   a4, <adiante>  ; se nulo, pula a chamada
callx8 a4             ; so entao chama
```

Ficou registrado aqui para não virar suspeita de novo.

## Alternativas avaliadas e não adotadas

Registradas aqui e no `main.cpp` para não serem reconsideradas do zero
mais tarde.

### Retentar o Wi-Fi sem reiniciar

**Propósito.** Se o roteador ficar fora do ar por muito tempo, o
`ESP.restart()` do `garantirWiFi()` vira um ciclo de reboot a cada ~40 s
(30 s de timeout + 10 s de espera), indefinidamente. A alternativa seria
insistir no Wi-Fi no próprio laço, sem nunca reiniciar.

**Solução proposta.** Trocar `println` + `delay` + `ESP.restart()` por um
rearme do timeout seguido de `WiFi.begin()` e `continue`. O trecho exato
está comentado no `main.cpp`, junto do `ESP.restart()`.

**Por que está inativa.**

1. O restart limpa fragmentação de heap acumulada. Num aparelho que roda
   por meses sem parar, isso é vantagem real: o reboot periódico é
   higiene, não efeito colateral.
2. O custo do reboot era desgaste de flash, porque cada `WiFi.begin()`
   escrevia na NVS. Com `WiFi.persistent(false)` no `setup()`, esse custo
   deixou de existir.
3. O estado perdido no reboot (`estado`, `tentativasWol`) é barato de
   reconstruir: o primeiro ciclo após o boot já pinga e redescobre se o
   alvo está no ar.

O ciclo de reboot não é defeito a corrigir — é o comportamento de
recuperação escolhido.

## Estado conhecido

- **Sem teste automatizado.** O firmware é simples o suficiente para
  validar por serial monitor.
- **Sem OTA.** Cada mudança exige cabo USB.
- **Nada é gravado em disco.** Existe a página de status e existe o
  histórico de 20 ocorrências, mas os dois vivem em RAM: o reinício os
  apaga, e o reinício de higiene acontece a cada 24 h. A única exceção é
  o contador de reinícios e o motivo do último, que ficam em RTC RAM e
  sobrevivem ao reset por software — mas não a queda de energia. Não há
  flash, cartão SD nem envio para fora: histórico de semanas não existe,
  e não há como reconstruir depois.
- **Não distingue "desligado" de "inalcançável por rede".** Se o alvo
  estiver ligado mas isolado — cabo solto, switch fora, firewall
  descartando ICMP, isolamento de clientes no Wi-Fi — a sentinela lê como
  desligado e manda WoL. São pacotes inofensivos, mas o diagnóstico fica
  invertido. Continua sendo um gap real: não há como fechá-lo só com
  ping.
- **Falha interna da checagem não cai mais nesse caso.** Isto deixou de
  ser verdade para as três falhas do próprio ESP32 dentro de
  `alvoResponde()`: elas não saem mais como "alvo caído", reiniciam o
  aparelho com o motivo gravado. A ambiguidade que restou é só a de
  rede, do item acima.
- **Credencial em texto claro no binário.** Inerente ao Arduino. Ver a
  seção Segurança.
- **Sem alarme.** Se o alvo nunca subir, o ESP32 tenta para sempre em
  silêncio. Não há notificação para fora.
- **Falha local reinicia, e o reinício zera o estado.** As três falhas da
  checagem de ping reiniciam o aparelho de propósito. O efeito colateral é
  que `estado` e `tentativasWol` se perdem: se o alvo estava offline
  há várias tentativas, o contador volta a zero e as retentativas rápidas
  recomeçam. Custo aceitável — o primeiro ciclo após o boot já pinga e
  redescobre a situação — mas explica um contador que reinicia sozinho no
  log.

## Verificações já feitas

- Compila limpo para `esp32dev` (RAM 14,3%, Flash 60,1%), sem warnings.
- Estrutura do magic packet validada byte a byte contra a spec do
  Wake-on-LAN: 102 bytes, 6x `0xFF` + 16 repetições do MAC, sem lacuna
  nem estouro de buffer.
- Conversão de `IPAddress` para o `ip4_addr_t` do lwIP conferida contra
  as macros reais (`LWIP_MAKEU32` + `PP_HTONL`): os dois lados produzem
  `0xC312A8C0` para 192.168.X.Y. Um erro de ordem de bytes aqui faria
  a sentinela pingar outro host sem avisar.
- Máquina de estados simulada em quatro cenários (já ligado no boot;
  sobe com 1 WoL; resiste a 5 WoL; cai durante o monitoramento). As
  transições e os intervalos batem com o especificado.
- Broadcast para `255.255.255.255` funciona sem `setsockopt(SO_BROADCAST)`
  porque o lwIP do ESP-IDF compila com `IP_SOF_BROADCAST = 0`.
- `udp.begin()` não é necessário: `beginPacket()` cria o socket sozinho.
- Toda sessão de ping é encerrada com `esp_ping_delete_session()`. Lendo
  o `ping_sock.c` do ESP-IDF, esse `delete` não libera nada na hora: só
  marca a sessão para encerrar. Quem libera é a tarefa interna do ping,
  que espera com prazo de 1 segundo e, ao ver a marca, sai devolvendo a
  memória, o buffer do pacote ICMP, o socket e a si mesma. Com 5 minutos
  até a próxima verificação, a margem é de 300 para 1 — não vaza.
- O socket UDP é fechado (`udp.stop()`) antes de cada reconexão de Wi-Fi.
  O `WiFiUDP` reaproveita o mesmo socket para sempre depois de criado
  (`beginPacket()` retorna cedo se `udp_server != -1`), então sem isso o
  magic packet sairia por um socket da sessão de rede anterior.
- Compila sem nenhum aviso sob `-Wall -Wextra`. Verificado com um teste
  de controle (uma variável não usada plantada de propósito) para
  confirmar que os avisos estavam mesmo ativos, e não silenciados.
- Constantes e mensagens da serial deste README conferidas contra o
  código, uma a uma.
- Teste adversarial do `.gitignore` com 28 vetores: cópias do `secrets.h`
  em variantes de sufixo (`.bak`, `_backup`, `.old`, `.orig`, `.save`,
  `copy`, `~`, `.swp`), de caixa (`SECRETS.h`, `Secrets.h`, `SeCrEtS.h`),
  em subdiretórios, mais `credentials.h`, `.env`, `.pem` e `.key`. Cada
  uma criada com uma senha real dentro; nenhuma foi versionada, e o
  `secrets.example.h` continuou publicado.
- Simulação de publicação com credencial real preenchida: nada de
  sensível entra no `git add -A`.
- **Dúvida de vazamento na sessão de ping: encerrada.** O fonte do
  `esp_ping` é público (`components/lwip/apps/ping/ping_sock.c`, no
  repositório [espressif/esp-idf](https://github.com/espressif/esp-idf))
  e foi lido. O `esp_ping_delete_session()` apenas marca a sessão; quem
  devolve memória, buffer do pacote ICMP, socket e a própria task e a
  tarefa interna do ping, em até ~1 s. Contra 5 min até a verificação
  seguinte, a margem é de 300 para 1. A mesma leitura confirmou que os
  callbacks não definidos (`on_ping_success`, `on_ping_timeout`) passam
  por checagem de ponteiro nulo antes da chamada indireta — o que antes
  só se sabia por desmontagem do `.obj` do toolchain.
- **Travas de compilação dos oito campos do `secrets.h`**, testadas uma a
  uma com o build falhando de propósito em cada caso: template intocado,
  SSID vazio, senha vazia, nome do alvo não editado, MAC de exemplo, IP
  do alvo de exemplo, IP do ESP32 de exemplo, gateway de exemplo, e
  máscara com número errado de octetos. A máscara `255, 255, 255, 0` foi
  testada em separado para confirmar que ela **compila** — sem esse
  teste, a ausência de trava de valor nela seria afirmação não
  verificada.
- **Caminho único de reinício.** Não existe `ESP.restart()` solto no
  `main.cpp`: os cinco motivos passam por `reiniciar()`, que grava o
  texto em RTC RAM antes de reiniciar. Conferido por varredura no
  arquivo.
- **Nenhuma constante órfã.** Os 30 nomes de constante e variável global
  do `main.cpp` foram conferidos um a um; todos têm pelo menos um uso
  além da declaração.
- Mensagens da serial extraídas do `main.cpp` e conferidas contra este
  README nos dois sentidos: mensagem no código que faltasse aqui, e
  mensagem daqui que não existisse mais no código.
- **Trava do `REINICIO_PERIODICO_MS` testada por regressão.** Subindo a
  constante para 45 dias, o build para com a mensagem do `static_assert`
  explicando que a comparação direta do `loop()` deixa de valer acima de
  ~40 dias e o que usar no lugar. Com 24 h, compila.
- **Avisos realmente ativos.** O "zero avisos sob `-Wall -Wextra`" foi
  confirmado com um teste de controle: uma variável não usada plantada de
  propósito no `loop()` produz `-Wunused-variable`. Sem esse controle,
  "zero avisos" poderia ser apenas flag desligada.
- `WiFi.macAddress()` funciona antes do `WiFi.mode()`: com o rádio em
  `WIFI_MODE_NULL` o core lê o MAC direto do efuse em vez de perguntar ao
  driver. Conferido no fonte do core, e é o que permite a linha
  `MAC do ESP32:` sair no bloco de boot, antes da rede existir.
