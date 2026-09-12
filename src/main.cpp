/*
  ESP32 - Sentinela de Wake-on-LAN
  --------------------------------
  Compilado e gravado via PlatformIO no VS Code / VSCodium.

  Funcao do dispositivo: manter uma maquina da rede ligada.

  Fluxo continuo:
    ONLINE   -> pinga o alvo a cada 5 min. Enquanto responder, so observa.
    OFFLINE  -> nao respondeu: envia magic packet, espera o boot, pinga de novo.
                Se ainda nao subiu, repete indefinidamente ate ligar.
                Quando ligar, volta para ONLINE.

  Por que ping por IP e nao por MAC: ICMP e camada 3 e exige um endereco IP;
  nao existe "pingar um MAC". O equivalente em camada 2 seria ARP, mas ARP e
  uma consulta indexada por IP (continua precisando do IP) e, pior, algumas
  NICs armadas para Wake-on-LAN respondem ARP com a maquina desligada -
  reportariam "ligado" para um servidor morto. ICMP exige a pilha de rede do
  SO no ar, que e exatamente a condicao que queremos detectar.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>   // vem no core do ESP32; nao exige lib_deps
#include "secrets.h"     // credenciais reais - fica fora do git

// API de ping nativa do ESP-IDF. Ja vem no liblwip.a do core,
// nao precisa de biblioteca externa em lib_deps.
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"

// ======================== CONFIGURACAO ==========================
//
// Esta secao NAO e o lugar de editar para instalar o aparelho em outra
// rede. Tudo que e especifico da instalacao - credenciais de Wi-Fi,
// nome/IP/MAC do alvo, endereco do proprio ESP32 - vive em
// src/secrets.h, que fica fora do git. O que vem abaixo apenas CONSOME
// esses valores, dando a eles um tipo e um nome utilizavel no resto do
// arquivo.
//
// O que de fato mora aqui, e nao no secrets.h, sao os parametros de
// COMPORTAMENTO: tempos, numero de repeticoes, tetos. Esses sao decisao
// do projeto e nao dado de quem instalou - valem igual em qualquer rede,
// e mudar um deles muda o que a sentinela faz, nao onde ela olha.
//
// Regra pratica para quem for mexer: se o valor muda de casa para casa,
// o lugar dele e o secrets.h. Se ele descreve como a sentinela se
// comporta, o lugar e aqui.
// ================================================================

// --- Identificacao do firmware ---
// EDITADA A MAO a cada versao significativa. Nao ha nada automatico
// atras disso: nem tag de git, nem numero de build. E deliberado - o
// aparelho e gravado por USB, de um clone que pode estar em qualquer
// ponto do historico, e um numero gerado automaticamente daria a
// impressao de rastreabilidade que nao existe. Este numero diz "que
// versao eu quis gravar"; o __DATE__/__TIME__ do banner diz "quando este
// binario foi feito", que e o dado objetivo.
const char* FIRMWARE_VERSAO = "1.0";

// --- Wi-Fi ---
// Credenciais vem de src/secrets.h (arquivo no .gitignore).
// Copie src/secrets.example.h se ainda nao existir.
// A rede precisa ser 2.4 GHz - o ESP32 nao fala 5 GHz.
const char* WIFI_SSID     = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;

// --- Alvo ---
// Os tres campos abaixo descrevem a maquina vigiada e vem do secrets.h,
// que fica fora do git: sao dados da rede de quem instalou, nao do
// projeto. Trocar os tres adapta a sentinela para outra maquina, sem
// tocar em logica nenhuma.

// So o nome usado nas mensagens da serial e na pagina de status.
const char* ALVO_NOME = SECRET_ALVO_NOME;

// MAC da interface cabeada do alvo (NAO e o MAC do ESP32).
// Windows: ipconfig /all   |   Linux: ip link
byte ALVO_MAC[6] = { SECRET_ALVO_MAC };

// EPISODIO REGISTRADO - nao ha codigo para reativar aqui.
//
// O projeto nasceu de um template de Wake-on-LAN que trazia o MAC de
// exemplo 00:1A:2B:3C:4D:5E embutido no firmware. Esse valor ficou no
// lugar do MAC real por engano, e a sentinela passou um tempo mandando
// magic packet para um endereco que nao existe na rede: o pacote saia
// perfeito, nada acontecia, e nao havia erro nenhum em lugar nenhum.
//
// Fica anotado porque o sintoma - WoL "funcionando" e alvo que nunca
// acorda - e dos mais dificeis de diagnosticar do zero. E foi esse
// episodio que motivou as travas de compilacao de SECRET_ALVO_MAC e
// SECRET_ALVO_IP, mais abaixo: hoje um MAC de exemplo esquecido nao
// chega a virar firmware.

// IP fixo do alvo, usado pelo ping. Precisa ser fixo: com DHCP o
// endereco muda e a sentinela passa a pingar outra maquina, ou nenhuma.
// Na instalacao que originou o projeto, e fixado por netplan no proprio
// servidor, sem reserva de DHCP no roteador.
IPAddress ALVO_IP(SECRET_ALVO_IP);

// --- Endereco do proprio ESP32 ---
// IP fixo para que a pagina de status tenha um endereco estavel. Com DHCP
// o IP muda sem aviso e a pagina "some" justamente quando alguem precisa
// dela. Ver o bloco em garantirWiFi() sobre como voltar a DHCP.
IPAddress ESP32_IP(SECRET_ESP32_IP);
IPAddress GATEWAY_IP(SECRET_GATEWAY_IP);
IPAddress MASCARA_REDE(SECRET_MASCARA_REDE);

// Broadcast "limitado": nao depende da faixa de IP da rede,
// entao trocar de roteador/provedor nao exige mexer aqui.
IPAddress BROADCAST_IP(255, 255, 255, 255);

const int WOL_PORT   = 9;   // porta padrao do Wake-on-LAN
const int REPETICOES = 3;   // manda o pacote 3x (UDP nao garante entrega)

// --- Tempos ---
const unsigned long WIFI_TIMEOUT_MS = 30000;        // 30 s para conectar no Wi-Fi

const unsigned long INTERVALO_MONITORAMENTO_MS = 5UL * 60UL * 1000UL;  // 5 min
const unsigned long ESPERA_POS_WOL_MS          = 90UL * 1000UL;        // 90 s
const unsigned long INTERVALO_RETENTATIVA_MS   = 5UL * 60UL * 1000UL;  // 5 min

// Quantas tentativas rapidas antes de espacar as retentativas. Evita
// martelar a rede quando o alvo esta fisicamente desligado da tomada,
// sem nunca desistir (o requisito e insistir ate ligar).
const int TENTATIVAS_RAPIDAS = 3;

// Reinicio periodico de higiene. Ver o bloco no inicio do loop().
const unsigned long REINICIO_PERIODICO_MS = 24UL * 60UL * 60UL * 1000UL;  // 24 h

// O teste no loop() e "millis() > REINICIO_PERIODICO_MS", comparacao
// direta. Ela so e valida porque a referencia e o boot, onde millis()
// vale 0, e porque 24 h esta muito longe do estouro de ~49,7 dias do
// unsigned long de 32 bits. Passado o estouro, millis() volta a zero e a
// comparacao direta deixa de significar "ja se passaram N ms".
//
// O teto de 40 dias abaixo existe para que aumentar a constante nao
// quebre isso em silencio: quem subir o valor alem dele para com erro de
// compilacao, e nao com um aparelho que reinicia na hora errada meses
// depois. Ver a ressalva no comentario de esperar().
static_assert(REINICIO_PERIODICO_MS < 40UL * 24UL * 60UL * 60UL * 1000UL,
              "REINICIO_PERIODICO_MS perto demais do estouro de millis() (~49,7 dias). "
              "Acima de ~40 dias a comparacao direta do loop() deixa de valer: "
              "troque por (millis() - referencia) > REINICIO_PERIODICO_MS, "
              "guardando a referencia do boot numa variavel.");

// --- Ping ---
const uint32_t PINGS_POR_CHECAGEM = 3;      // considera online com 1 resposta
const uint32_t PING_TIMEOUT_MS    = 1000;
const uint32_t PING_INTERVALO_MS  = 500;

// ================================================================

// Trava de compilacao: impede gravar um firmware sem credenciais uteis.
// Sem isso o erro so apareceria como uma falha de conexao silenciosa na
// serial, depois de a placa ja estar gravada.
//
// Sao duas checagens por campo, porque cada uma pega um descuido diferente:
//   - mesmaString: copiou o secrets.example.h e esqueceu de editar
//   - vazia:       apagou o valor e deixou "" (a string vazia passava
//                  pela checagem de placeholder, entao o build seguia
//                  e gerava firmware com SSID e senha em branco)
static bool constexpr mesmaString(const char* a, const char* b) {
  return *a == *b && (*a == '\0' || mesmaString(a + 1, b + 1));
}
static bool constexpr vazia(const char* s) {
  return *s == '\0';
}

static_assert(!mesmaString(SECRET_WIFI_SSID, "PREENCHER_SSID_AQUI"),
              "Preencha SECRET_WIFI_SSID em src/secrets.h antes de compilar.");
static_assert(!vazia(SECRET_WIFI_SSID),
              "SECRET_WIFI_SSID esta vazio em src/secrets.h. Nao existe rede sem nome.");

static_assert(!mesmaString(SECRET_WIFI_PASSWORD, "PREENCHER_SENHA_AQUI"),
              "Preencha SECRET_WIFI_PASSWORD em src/secrets.h antes de compilar.");
// Rede aberta (sem senha) e o unico caso legitimo de senha vazia. Se for
// esse o seu caso, comente o static_assert abaixo - mas note que uma rede
// aberta e escolha ruim para um aparelho que fica ligado permanentemente.
static_assert(!vazia(SECRET_WIFI_PASSWORD),
              "SECRET_WIFI_PASSWORD esta vazia em src/secrets.h. Veja o comentario acima se a rede for aberta.");

static_assert(!mesmaString(SECRET_ALVO_NOME, "PREENCHER_NOME_DO_ALVO"),
              "Preencha SECRET_ALVO_NOME em src/secrets.h antes de compilar.");
static_assert(!vazia(SECRET_ALVO_NOME),
              "SECRET_ALVO_NOME esta vazio em src/secrets.h.");

// MAC e IP tambem tem trava, e sao os dois campos que mais precisam
// dela: MAC errado nao gera erro nenhum em execucao (o magic packet sai
// perfeito para um endereco que nao existe) e IP errado faz a sentinela
// concluir que o alvo vive desligado. Este projeto ja foi vitima desse
// exato bug - ver o registro no topo do arquivo.
//
// O tamanho dos vetores abaixo e DEDUZIDO da lista do secrets.h, e nao
// fixado em 6 e 4. E isso que permite pegar um MAC com cinco bytes, que
// de outra forma compilaria e teria o sexto preenchido com zero em
// silencio.
constexpr byte    MAC_CONFERENCIA[] = { SECRET_ALVO_MAC };
constexpr uint8_t IP_CONFERENCIA[]  = { SECRET_ALVO_IP };

static_assert(sizeof(MAC_CONFERENCIA) == 6,
              "SECRET_ALVO_MAC precisa ter exatamente 6 bytes em src/secrets.h.");
static_assert(sizeof(IP_CONFERENCIA) == 4,
              "SECRET_ALVO_IP precisa ter exatamente 4 octetos em src/secrets.h.");

// Comparacao byte a byte com os valores de exemplo do secrets.example.h,
// para pegar o "copiei o template e esqueci de editar".
constexpr bool mesmoVetor(const uint8_t* a, const uint8_t* b, int n) {
  return n == 0 || (*a == *b && mesmoVetor(a + 1, b + 1, n - 1));
}

constexpr uint8_t MAC_EXEMPLO[] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
constexpr uint8_t IP_EXEMPLO[]  = { 192, 168, 1, 100 };

static_assert(!mesmoVetor(MAC_CONFERENCIA, MAC_EXEMPLO, 6),
              "SECRET_ALVO_MAC ainda e o exemplo. Troque em src/secrets.h pelo MAC real do alvo.");
static_assert(!mesmoVetor(IP_CONFERENCIA, IP_EXEMPLO, 4),
              "SECRET_ALVO_IP ainda e o exemplo. Troque em src/secrets.h pelo IP real do alvo.");

// Mesmas travas para os enderecos do proprio aparelho.
constexpr uint8_t ESP32_CONFERENCIA[]   = { SECRET_ESP32_IP };
constexpr uint8_t GATEWAY_CONFERENCIA[] = { SECRET_GATEWAY_IP };
constexpr uint8_t MASCARA_CONFERENCIA[] = { SECRET_MASCARA_REDE };

static_assert(sizeof(ESP32_CONFERENCIA) == 4,
              "SECRET_ESP32_IP precisa ter exatamente 4 octetos em src/secrets.h.");
static_assert(sizeof(GATEWAY_CONFERENCIA) == 4,
              "SECRET_GATEWAY_IP precisa ter exatamente 4 octetos em src/secrets.h.");
static_assert(sizeof(MASCARA_CONFERENCIA) == 4,
              "SECRET_MASCARA_REDE precisa ter exatamente 4 octetos em src/secrets.h.");

constexpr uint8_t ESP32_EXEMPLO[]   = { 192, 168, 1, 197 };
constexpr uint8_t GATEWAY_EXEMPLO[] = { 192, 168, 1, 1 };

static_assert(!mesmoVetor(ESP32_CONFERENCIA, ESP32_EXEMPLO, 4),
              "SECRET_ESP32_IP ainda e o exemplo. Escolha em src/secrets.h um IP livre, "
              "fora da faixa de DHCP do roteador.");
static_assert(!mesmoVetor(GATEWAY_CONFERENCIA, GATEWAY_EXEMPLO, 4),
              "SECRET_GATEWAY_IP ainda e o exemplo. Troque em src/secrets.h pelo IP do seu roteador.");

// A mascara e a UNICA sem trava de valor, de proposito. 255.255.255.0 e a
// mascara legitima da grande maioria das redes domesticas: rejeitar o
// valor de exemplo daria falso positivo em quase toda instalacao real, e
// obrigaria a inventar um valor "nao-exemplo" que provavelmente estaria
// errado. So a quantidade de octetos e conferida.

// Distingue a PRIMEIRA conexao de uma reconexao. Sem isso o boot
// anunciava "Wi-Fi desconectado" e gravava "Wi-Fi caiu" no historico,
// descrevendo uma queda que nunca houve - a mesma funcao serve os dois
// casos, e ate aqui ela so sabia dizer um deles. Comeca falsa a cada
// boot de proposito: depois de um reinicio, a conexao seguinte e mesmo
// uma primeira conexao daquele ciclo de vida.
bool jaConectouWiFi = false;

WiFiUDP udp;
WebServer server(80);

// Prototipo. A definicao esta la embaixo, junto das outras funcoes do
// servidor web, mas alvoResponde() e esperar() chamam atenderWeb() bem
// antes disso no arquivo - e .cpp nao tem prototipagem automatica.
void atenderWeb();

enum EstadoAlvo { DESCONHECIDO, ONLINE, OFFLINE };
EstadoAlvo estado = DESCONHECIDO;
int tentativasWol = 0;

// Momento da ultima transicao de estado, para a pagina dizer ha quanto
// tempo o alvo esta como esta.
unsigned long estadoDesde       = 0;
unsigned long ultimaVerificacao = 0;
unsigned long proximaEspera     = 0;
int totalWolDesdeBoot           = 0;

// ---------------------------------------------------------------
// Historico de ocorrencias
// ---------------------------------------------------------------
// Vetor circular de tamanho FIXO, reservado uma vez e imutavel dali em
// diante. Sem String, sem new, sem malloc, sem std::vector.
//
// A razao e direta: este aparelho fica meses ligado registrando eventos.
// Texto de tamanho dinamico neste caminho significaria alocar e liberar
// blocos de tamanhos variados milhares de vezes, que e a receita de
// fragmentacao de heap - exatamente o problema de memoria que o projeto
// passou uma revisao inteira descartando. Nao faz sentido reintroduzi-lo
// pela porta dos fundos so para ter um log bonito.
//
// Entram aqui apenas OCORRENCIAS, nunca o tique de rotina. Com o
// "continua online" de cada ciclo, as 20 posicoes se esgotariam em 100
// minutos e o historico nao serviria para nada.
const int HISTORICO_TAMANHO = 20;
const int HISTORICO_TEXTO   = 56;

struct Ocorrencia {
  unsigned long quando;
  char texto[HISTORICO_TEXTO];
};

Ocorrencia historico[HISTORICO_TAMANHO];
int historicoProximo = 0;   // onde a proxima entrada sera escrita
int historicoTotal   = 0;   // quantas ja foram gravadas (satura em HISTORICO_TAMANHO)

// printf-like, mas escrevendo direto no espaco ja reservado. O snprintf
// trunca no limite do buffer em vez de estourar.
void registrar(const char* formato, ...) {
  Ocorrencia& o = historico[historicoProximo];
  o.quando = millis();

  va_list args;
  va_start(args, formato);
  vsnprintf(o.texto, HISTORICO_TEXTO, formato, args);
  va_end(args);

  historicoProximo = (historicoProximo + 1) % HISTORICO_TAMANHO;
  if (historicoTotal < HISTORICO_TAMANHO) historicoTotal++;
}

// ---------------------------------------------------------------
// Reinicio com o motivo preservado
// ---------------------------------------------------------------
// O historico acima vive na RAM comum, entao um reinicio o apaga. Isso
// deixava um buraco justamente no que mais importa: os eventos que
// disparam o reinicio eram os unicos que nunca chegavam a aparecer na
// pagina, porque o proprio reinicio que eles anunciavam os destruia.
//
// A solucao aqui e minima de proposito - so o contador e o motivo do
// ultimo reinicio, em RTC RAM, que sobrevive ao reset por software.
// Mover o historico inteiro para ca traria o problema dos horarios:
// millis() zera no boot, e as entradas antigas passariam a mentir sobre
// quando aconteceram.
//
// A RTC RAM NAO sobrevive a queda de energia, e essa e a semantica
// desejada: falta de luz nao e sintoma de defeito, e o contador deve
// mesmo voltar a zero. A palavra magica distingue dado nosso de lixo
// depois de um power-on, quando a regiao vem com qualquer coisa.
const uint32_t RTC_MAGIA = 0x5E4E7114;

// RTC_NOINIT_ATTR, e NAO RTC_DATA_ATTR. A diferenca nao aparece no nome
// e custou um defeito silencioso: RTC_DATA_ATTR poe a variavel na secao
// .rtc.data, que e PROGBITS e faz parte de um segmento LOAD do binario -
// ou seja, o bootloader a recopia da flash a CADA boot, por cima do que
// estava na RTC RAM. Ela sobrevive ao deep sleep, que nao usamos, e nao
// sobrevive ao reset, que e o unico caso que nos interessa. O resultado
// era o contador preso em zero para sempre: a palavra magica nunca casava,
// porque nunca havia lixo para distinguir - havia sempre zero vindo da
// flash. RTC_NOINIT_ATTR poe na .rtc_noinit, que e NOBITS e fica fora de
// qualquer LOAD: ninguem escreve nela no boot, entao o valor atravessa o
// reset e vem com lixo de verdade depois de um power-on, que e exatamente
// o que a palavra magica acima existe para peneirar.
//
// Se um dia isso voltar para RTC_DATA_ATTR, o contador volta a mentir sem
// emitir erro nenhum. Para conferir: xtensa-esp32-elf-readelf -S no .elf,
// e os simbolos tem que cair em .rtc_noinit.
RTC_NOINIT_ATTR uint32_t rtcMagia;
RTC_NOINIT_ATTR uint32_t rtcReinicios;
RTC_NOINIT_ATTR char     rtcMotivo[48];

// Caminho unico de reinicio: nenhum ESP.restart() solto no resto do
// arquivo. Assim nao existe reinicio sem motivo registrado.
void reiniciar(const char* motivo) {
  snprintf(rtcMotivo, sizeof(rtcMotivo), "%s", motivo);
  rtcReinicios++;
  rtcMagia = RTC_MAGIA;
  Serial.flush();
  delay(100);
  ESP.restart();
}

// ---------------------------------------------------------------
// Wake-on-LAN
// ---------------------------------------------------------------

// Retorna true so se a pilha de rede aceitou o pacote. Sem esse
// retorno o programa imprimia "enviado" mesmo quando o envio falhava.
bool enviarMagicPacket() {
  byte pacote[102];

  // 6 bytes 0xFF = assinatura fixa que identifica um magic packet
  for (int i = 0; i < 6; i++) {
    pacote[i] = 0xFF;
  }

  // MAC do alvo repetido 16x (6 x 16 = 96 bytes)
  for (int i = 0; i < 16; i++) {
    memcpy(&pacote[6 + i * 6], ALVO_MAC, 6);
  }

  if (udp.beginPacket(BROADCAST_IP, WOL_PORT) != 1) {
    return false;
  }
  udp.write(pacote, sizeof(pacote));
  return udp.endPacket() == 1;
}

// Dispara o magic packet REPETICOES vezes. Retorna quantas sairam.
int acordarAlvo() {
  int enviados = 0;
  for (int i = 0; i < REPETICOES; i++) {
    if (enviarMagicPacket()) enviados++;
    delay(200);
  }
  return enviados;
}

// ---------------------------------------------------------------
// Ping (ICMP) - wrapper bloqueante sobre a API assincrona do ESP-IDF
// ---------------------------------------------------------------

static volatile bool     pingFinalizado = false;
static volatile uint32_t pingRespostas  = 0;

static void aoFinalizarPing(esp_ping_handle_t hdl, void* args) {
  uint32_t recebidas = 0;
  esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &recebidas, sizeof(recebidas));
  pingRespostas  = recebidas;
  pingFinalizado = true;
}

// true = o alvo respondeu pelo menos um ICMP echo.
// Uma unica perda de pacote nao derruba o diagnostico: sao
// PINGS_POR_CHECAGEM tentativas e basta uma resposta.
//
// O false desta funcao tem um significado unico: o alvo nao
// respondeu. Falha do proprio ESP32 nunca sai por aqui como false -
// nesses casos o aparelho reinicia. A razao e que quem chama usa o
// false como "o alvo esta desligado" e reage mandando Wake-on-LAN;
// se um defeito local virasse esse mesmo false, a sentinela passaria
// a martelar WoL para sempre num alvo que talvez esteja no ar,
// e o log culparia o alvo por um problema que e daqui.
bool alvoResponde() {
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();

  ip_addr_t alvo;
  memset(&alvo, 0, sizeof(alvo));
  alvo.type = IPADDR_TYPE_V4;
  // IPAddress e ip4_addr_t guardam os octetos na mesma ordem,
  // entao o cast direto e valido nas duas pontas.
  alvo.u_addr.ip4.addr = (uint32_t)ALVO_IP;

  cfg.target_addr = alvo;
  cfg.count       = PINGS_POR_CHECAGEM;
  cfg.timeout_ms  = PING_TIMEOUT_MS;
  cfg.interval_ms = PING_INTERVALO_MS;

  esp_ping_callbacks_t cbs = {};
  cbs.on_ping_end = aoFinalizarPing;

  pingFinalizado = false;
  pingRespostas  = 0;

  esp_ping_handle_t hdl = NULL;
  if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK || hdl == NULL) {
    Serial.println("  [erro] nao foi possivel criar a sessao de ping");
    // Nao da para criar a sessao quando falta heap ou nao ha socket
    // livre - as duas coisas sao doenca do ESP32, e nenhuma delas diz
    // nada sobre o alvo. Seguir daqui como se fosse "alvo caido"
    // renderia WoL a toa e um log mentiroso. Pior: se for falta de
    // heap, o quadro so piora a cada ciclo. Reiniciar devolve a memoria
    // e os sockets ao estado inicial, que e o unico jeito de sair
    // dessa daqui de dentro.
    reiniciar("falha ao criar a sessao de ping");
    return false;   // inalcancavel: reiniciar() nao retorna
  }

  if (esp_ping_start(hdl) != ESP_OK) {
    Serial.println("  [erro] nao foi possivel iniciar a sessao de ping");
    // A sessao existe mas nao arrancou: sobrou a task, o timer ou algum
    // recurso interno. De novo, defeito local e nao diagnostico do alvo.
    // Sem stop() aqui porque nada chegou a rodar - so o delete, para nao
    // deixar a sessao pendurada antes do reboot.
    esp_ping_delete_session(hdl);
    reiniciar("falha ao iniciar a sessao de ping");
    return false;   // inalcancavel: reiniciar() nao retorna
  }

  // Teto de seguranca: se o callback nunca vier, nao trava o loop.
  // A folga de 5000UL e generosa de proposito. Com uma folga apertada,
  // um atraso benigno estouraria o prazo e seria confundido com defeito;
  // com esta, estourar significa que a task do ping realmente nao
  // terminou, e ai o reboot abaixo se justifica.
  const unsigned long teto =
      PINGS_POR_CHECAGEM * (PING_TIMEOUT_MS + PING_INTERVALO_MS) + 5000UL;
  const unsigned long inicio = millis();
  while (!pingFinalizado && (millis() - inicio) < teto) {
    atenderWeb();
    delay(50);
  }

  if (!pingFinalizado) {
    Serial.println("  [erro] a sessao de ping nao terminou dentro do prazo");
    // O callback nunca veio. A task do ping travou ou morreu, e sem ela
    // pingRespostas fica em zero para sempre - o que sairia daqui como
    // "alvo desligado" em toda checagem seguinte, indefinidamente.
    // E o pior dos tres casos justamente por ser silencioso: a sentinela
    // continuaria "funcionando", so que cega. Encerra a sessao pelo
    // caminho normal e reinicia, que e o que recupera a task.
    esp_ping_stop(hdl);
    esp_ping_delete_session(hdl);
    reiniciar("ping sem retorno dentro do prazo");
    return false;   // inalcancavel: reiniciar() nao retorna
  }

  // Obrigatorio: sem o delete a sessao vaza memoria a cada checagem,
  // e este dispositivo roda por meses sem reiniciar.
  esp_ping_stop(hdl);
  esp_ping_delete_session(hdl);

  return pingRespostas > 0;
}

// ---------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------

// Mantem a conexao viva. O Wi-Fi cai eventualmente em qualquer
// dispositivo que fica meses ligado; sem isso o ping falharia e o
// firmware acharia que o alvo caiu, mandando WoL a toa.
void garantirWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  if (jaConectouWiFi) {
    Serial.println("Wi-Fi desconectado. Reconectando...");
    registrar("Wi-Fi caiu - reconectando");
  } else {
    Serial.println("Conectando no Wi-Fi...");
    registrar("Conectando no Wi-Fi");
  }

  // Fecha o socket UDP antes de reconectar. O WiFiUDP reaproveita o
  // mesmo socket para sempre depois de criado (beginPacket retorna cedo
  // se udp_server != -1), entao sem este stop() o magic packet
  // continuaria saindo por um socket criado na sessao de rede anterior.
  udp.stop();

  WiFi.disconnect();

  // ---------------- IP FIXO - inicio do bloco ----------------
  // Fica aqui, e nao so no setup(), para valer tambem em toda reconexao:
  // assim a configuracao nao depende da ordem das chamadas la em cima.
  //
  // PARA VOLTAR A DHCP: comente a linha WiFi.config abaixo. O aparelho
  // passa a pegar endereco do roteador, e o IP obtido aparece na serial
  // logo depois de "Wi-Fi OK. IP do ESP32:". A pagina de status continua
  // funcionando, so que num endereco que pode mudar sem aviso.
  //
  // O ESP32_IP precisa estar FORA da faixa de DHCP do roteador. Dentro
  // dela, o roteador pode entregar o mesmo endereco a outro aparelho e
  // criar conflito - os dois somem da rede de forma intermitente, que e
  // dos sintomas mais chatos de diagnosticar.
  WiFi.config(ESP32_IP, GATEWAY_IP, MASCARA_REDE, GATEWAY_IP);
  // ---------------- IP FIXO - fim do bloco -------------------

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - inicio > WIFI_TIMEOUT_MS) {
      Serial.println();
      Serial.println("FALHA: nao conectou no Wi-Fi dentro do timeout.");
      Serial.println("Confira SSID/senha em src/secrets.h.");
      Serial.println("Confira tambem se a rede e 2.4 GHz (o ESP32 nao fala 5 GHz).");
      Serial.println("Reiniciando em 10s para tentar de novo...");
      Serial.flush();
      delay(10000);
      // dispositivo sem operador: tem que se recuperar sozinho
      reiniciar("Wi-Fi nao conectou dentro do prazo");

      // ----------------------------------------------------------------
      // ALTERNATIVA AVALIADA E MANTIDA INATIVA: retentar sem reiniciar
      // ----------------------------------------------------------------
      // PROPOSITO
      //   Se o roteador ficar fora do ar por muito tempo, o ESP.restart()
      //   acima vira um ciclo de reboot a cada ~40s (30s de timeout + 10s
      //   de espera), indefinidamente. A alternativa seria insistir no
      //   Wi-Fi aqui mesmo, sem nunca reiniciar.
      //
      // SOLUCAO PROPOSTA
      //   Trocar as tres linhas acima (println + delay + ESP.restart) por:
      //
      //     inicio = millis();          // rearma o timeout e continua
      //     WiFi.disconnect();
      //     WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      //     continue;
      //
      //   'inicio' precisa deixar de ser const para isso funcionar.
      //
      // POR QUE ESTA INATIVA
      //   1. O restart limpa fragmentacao de heap acumulada. Num aparelho
      //      que roda por meses sem parar, isso e uma vantagem real, nao
      //      um efeito colateral: o reboot periodico e higiene.
      //   2. O custo que o reboot tinha era desgaste de flash, porque
      //      cada WiFi.begin() escrevia na NVS. Com o WiFi.persistent(false)
      //      no setup(), esse custo deixou de existir.
      //   3. O estado perdido no reboot (estado, tentativasWol) e barato
      //      de reconstruir: o primeiro ciclo apos o boot ja pinga e
      //      redescobre se o alvo esta no ar.
      //
      //   Ou seja: o ciclo de reboot nao e um defeito a corrigir, e o
      //   comportamento de recuperacao escolhido. Este bloco fica aqui
      //   para registrar que a alternativa foi considerada, e por que
      //   nao foi adotada.
      // ----------------------------------------------------------------
    }
    delay(300);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("Wi-Fi OK. IP do ESP32: ");
  Serial.println(WiFi.localIP());
  if (jaConectouWiFi) {
    registrar("Wi-Fi reconectado");
  } else {
    registrar("Wi-Fi conectado");
  }
  jaConectouWiFi = true;

  // O socket de escuta do servidor nao sobrevive a queda da interface -
  // mesmo motivo que ja obriga o udp.stop() logo acima. Sem este par
  // stop/begin a pagina para de abrir depois da primeira reconexao, e
  // ninguem descobre ate o dia em que for precisar dela.
  server.stop();
  server.begin();
}

// Espera em blocos curtos em vez de um delay() unico de 5 minutos, para
// que o servidor web seja atendido durante a espera. Com blocos de 1s a
// pagina demorava ate um segundo para responder cada requisicao, o que
// na pratica a fazia parecer travada.
// A subtracao de unsigned long trata o overflow de millis()
// (~49 dias) corretamente - por isso nao se compara millis() > alvo.
//
// EXCECAO, e e a unica no arquivo: o reinicio periodico, no inicio do
// loop(), compara millis() > REINICIO_PERIODICO_MS direto. Ali a
// comparacao vale porque a referencia nao e um instante qualquer, e sim
// o boot, onde millis() e exatamente 0 - a subtracao seria por zero e
// nao mudaria nada. E vale tambem porque 24 h esta muito longe do
// estouro: o aparelho reinicia e millis() recomeca do zero muito antes
// de chegar perto dos ~49,7 dias. Fora essas duas condicoes juntas a
// regra de cima continua valendo, e por isso ha um static_assert junto
// da constante travando o valor bem abaixo do estouro.
void esperar(unsigned long ms) {
  const unsigned long inicio = millis();
  while (millis() - inicio < ms) {
    atenderWeb();
    delay(50);
  }
}

void imprimirMacAlvo() {
  for (int i = 0; i < 6; i++) {
    if (ALVO_MAC[i] < 0x10) Serial.print("0");
    Serial.print(ALVO_MAC[i], HEX);
    if (i < 5) Serial.print(":");
  }
}

// ---------------------------------------------------------------
// Pagina de status
// ---------------------------------------------------------------
// Sem JavaScript e sem recurso externo de proposito: a pagina precisa
// abrir mesmo com a internet fora, que e justamente quando alguem vai
// querer olhar o estado do servidor de casa. O meta refresh basta.
//
// Os buffers abaixo sao locais e de tamanho fixo, pela mesma razao do
// historico: nada de String nesta rota, que pode ser chamada muitas
// vezes por minuto se alguem deixar a aba aberta.

// O WebServer so processa requisicao quando handleClient() e chamado.
// Como o laco principal passa ate 5 minutos parado esperando, sem isso a
// pagina responderia apenas nas frestas entre as esperas e pareceria
// quebrada. Por isso esta funcao e chamada de dentro de cada laco que
// dorme.
void atenderWeb() {
  server.handleClient();
}

// Escreve "3d 4h 12min" em buf. Omite as unidades maiores quando zero.
void formatarDuracao(char* buf, size_t tam, unsigned long ms) {
  unsigned long s = ms / 1000UL;
  unsigned long d = s / 86400UL;
  unsigned long h = (s % 86400UL) / 3600UL;
  unsigned long m = (s % 3600UL) / 60UL;
  if (d > 0)      snprintf(buf, tam, "%lud %luh %lumin", d, h, m);
  else if (h > 0) snprintf(buf, tam, "%luh %lumin", h, m);
  else            snprintf(buf, tam, "%lumin", m);
}

const char* nomeEstado() {
  switch (estado) {
    case ONLINE:  return "ONLINE";
    case OFFLINE: return "OFFLINE";
    default:      return "verificando";
  }
}

// Envia um pedaco de HTML sem criar String. O overload (const char*,
// size_t) do WebServer escreve direto no socket; a versao que recebe
// String alocaria e liberaria um bloco a cada chamada.
//
// O nome traz o "Html" porque num arquivo cujo assunto principal e
// disparar pacote de rede, uma funcao chamada so "enviar" ao lado de
// enviarMagicPacket() se le errado.
inline void enviarHtml(const char* s) {
  server.sendContent(s, strlen(s));
}

void paginaStatus() {
  const unsigned long agora = millis();
  // Buffer unico, reaproveitado a cada pedaco. 320 e folgado de proposito:
  // o snprintf trunca em SILENCIO, e o corte cai no meio de uma tag HTML,
  // quebrando a pagina sem nenhum sinal de erro. Alem disso ALVO_NOME vem
  // do secrets.h, onde nada limita o tamanho - um nome de maquina comprido
  // consome a margem sozinho.
  //
  // Quem acrescentar linhas na tabela abaixo precisa reconferir este valor:
  // o maior snprintf daqui ja usa ~176 bytes so de HTML literal.
  char buf[320];
  char t1[32], t2[32];

  // Transmissao em blocos (chunked): a pagina nunca existe inteira na
  // memoria. Montar tudo numa String antes de enviar pediria ~4 KB de
  // heap por requisicao, e com o refresh de 10s isso seria dezenas de
  // milhares de alocacoes por dia - o churn que o resto do projeto evita.
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");

  enviarHtml("<!DOCTYPE html><html lang='pt-br'><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<meta http-equiv='refresh' content='10'>"
         "<title>Sentinela de Wake-on-LAN</title><style>"
         "body{font-family:system-ui,sans-serif;margin:0;padding:16px;"
         "background:#12141a;color:#e6e6e6;line-height:1.5}"
         "h1{font-size:1.1rem;margin:0 0 4px}"
         "h2{font-size:.8rem;text-transform:uppercase;letter-spacing:.05em;"
         "color:#8a93a6;margin:22px 0 6px;font-weight:600}"
         ".big{font-size:1.7rem;font-weight:700;margin:6px 0 0}"
         ".on{color:#4ade80}.off{color:#f87171}.unk{color:#facc15}"
         "table{width:100%;border-collapse:collapse;font-size:.9rem}"
         "td{padding:6px 0;border-bottom:1px solid #262a35;vertical-align:top}"
         "td:first-child{color:#8a93a6;width:48%}"
         "ul{padding:0;margin:0}"
         "li{font-size:.85rem;padding:6px 0;border-bottom:1px solid #262a35;"
         "list-style:none}"
         ".t{color:#8a93a6;font-variant-numeric:tabular-nums}"
         ".nota{color:#6b7280;font-size:.75rem;margin-top:6px}"
         "</style></head><body><h1>Sentinela de Wake-on-LAN</h1>");

  // Estado atual e ha quanto tempo
  formatarDuracao(t1, sizeof(t1), agora - estadoDesde);
  const char* cor = (estado == ONLINE) ? "on" : (estado == OFFLINE ? "off" : "unk");
  snprintf(buf, sizeof(buf),
           "<div class='big %s'>%s</div><div class='t'>ha %s</div>",
           cor, nomeEstado(), t1);
  enviarHtml(buf);

  // Verificacao
  enviarHtml("<h2>Verificacao</h2><table>");
  formatarDuracao(t1, sizeof(t1), agora - ultimaVerificacao);
  const unsigned long decorrido = agora - ultimaVerificacao;
  if (proximaEspera > decorrido) {
    formatarDuracao(t2, sizeof(t2), proximaEspera - decorrido);
  } else {
    snprintf(t2, sizeof(t2), "agora");
  }
  snprintf(buf, sizeof(buf),
           "<tr><td>Ultima</td><td>ha %s</td></tr>"
           "<tr><td>Proxima em</td><td>%s</td></tr>"
           "<tr><td>WoL desde a ultima subida</td><td>%d</td></tr>"
           "<tr><td>WoL desde o boot</td><td>%d</td></tr></table>",
           t1, t2, tentativasWol, totalWolDesdeBoot);
  enviarHtml(buf);

  // ESP32
  enviarHtml("<h2>ESP32</h2><table>");

  // Mesma informacao do banner de boot, para quem so tem a pagina a mao.
  // Vale a ressalva do setup(): __DATE__ e __TIME__ congelam no momento
  // em que o main.cpp e compilado, e uma compilacao incremental que nao
  // o recompile carrega o carimbo antigo para dentro do binario novo.
  // Por isso o build de validacao do projeto e sempre do zero.
  snprintf(buf, sizeof(buf),
           "<tr><td>Firmware</td><td>%s<br><span class='t'>compilado em %s %s"
           "</span></td></tr>",
           FIRMWARE_VERSAO, __DATE__, __TIME__);
  enviarHtml(buf);

  formatarDuracao(t1, sizeof(t1), agora);
  snprintf(buf, sizeof(buf),
           "<tr><td>Ligado ha</td><td>%s</td></tr>"
           "<tr><td>Heap livre</td><td>%u B</td></tr>"
           "<tr><td>Minimo desde o boot</td><td>%u B</td></tr>",
           t1, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
  enviarHtml(buf);
  snprintf(buf, sizeof(buf),
           "<tr><td>IP</td><td>%s</td></tr>"
           "<tr><td>MAC</td><td>%s</td></tr>",
           WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str());
  enviarHtml(buf);

  if (rtcReinicios == 0) {
    enviarHtml("<tr><td>Reinicios</td><td>nenhum desde a ultima queda de "
               "energia</td></tr></table>");
  } else {
    snprintf(buf, sizeof(buf),
             "<tr><td>Reinicios</td><td>%u desde a ultima queda de energia"
             "<br>ultimo: %s</td></tr></table>",
             (unsigned)rtcReinicios, rtcMotivo);
    enviarHtml(buf);
  }
  enviarHtml("<div class='nota'>O minimo e o pior momento de memoria livre "
             "desde que o aparelho ligou.</div>");

  // Alvo
  snprintf(buf, sizeof(buf),
           "<h2>%s</h2><table>"
           "<tr><td>IP</td><td>%s</td></tr>"
           "<tr><td>MAC</td><td>%02X:%02X:%02X:%02X:%02X:%02X</td></tr></table>",
           ALVO_NOME, ALVO_IP.toString().c_str(),
           ALVO_MAC[0], ALVO_MAC[1], ALVO_MAC[2],
           ALVO_MAC[3], ALVO_MAC[4], ALVO_MAC[5]);
  enviarHtml(buf);

  // Ocorrencias, da mais recente para a mais antiga
  enviarHtml("<h2>Ocorrencias</h2><ul>");
  if (historicoTotal == 0) {
    enviarHtml("<li>nenhuma ainda</li>");
  } else {
    for (int i = 0; i < historicoTotal; i++) {
      int idx = (historicoProximo - 1 - i + HISTORICO_TAMANHO * 2) % HISTORICO_TAMANHO;
      formatarDuracao(t1, sizeof(t1), agora - historico[idx].quando);
      snprintf(buf, sizeof(buf),
               "<li><span class='t'>ha %s</span><br>%s</li>",
               t1, historico[idx].texto);
      enviarHtml(buf);
    }
  }
  enviarHtml("</ul></body></html>");

  server.sendContent("", 0);   // encerra o chunked
}

// ---------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("=== Sentinela de Wake-on-LAN ===");

  // Carimbo de versao. __DATE__ e __TIME__ sao substituidos pelo
  // pre-processador no momento em que ESTE arquivo e compilado - e so
  // entao. Uma compilacao incremental que nao recompile o main.cpp deixa
  // o carimbo antigo no binario novo, o que faria a placa mentir sobre a
  // propria idade justamente quando se esta tentando descobrir qual
  // firmware ela tem. Por isso o build de validacao do projeto e sempre
  // do zero: e o que garante que este carimbo diz a verdade.
  Serial.print("Firmware ");
  Serial.print(FIRMWARE_VERSAO);
  Serial.print(", compilado em ");
  Serial.print(__DATE__);
  Serial.print(" ");
  Serial.println(__TIME__);

  // Le o que sobreviveu ao ultimo reinicio. Lixo de power-on nao passa
  // pela palavra magica, e ai a contagem recomeca - que e o certo: queda
  // de energia nao e sintoma de defeito.
  if (rtcMagia != RTC_MAGIA) {
    rtcMagia     = RTC_MAGIA;
    rtcReinicios = 0;
    rtcMotivo[0] = '\0';
  } else if (rtcReinicios > 0) {
    Serial.print("Reinicios desde a ultima queda de energia: ");
    Serial.println(rtcReinicios);
    Serial.print("Motivo do ultimo: ");
    Serial.println(rtcMotivo);
    // Reinjeta no historico para o evento aparecer na pagina. Sem isso o
    // motivo morreria com a RAM que o proprio reinicio apagou.
    registrar("Reiniciou: %s", rtcMotivo);
  }
  Serial.print("Alvo: ");
  Serial.print(ALVO_NOME);
  Serial.print("  ");
  Serial.print(ALVO_IP);
  Serial.print("  ");
  imprimirMacAlvo();
  Serial.println();
  Serial.print("Intervalo de monitoramento: ");
  Serial.print(INTERVALO_MONITORAMENTO_MS / 60000UL);
  Serial.println(" min");

  // MAC do proprio ESP32. Serve para criar reserva de DHCP no roteador e
  // para identificar o aparelho na lista de clientes - sem isso ele fica
  // como mais um dispositivo sem nome no meio dos outros.
  //
  // Funciona antes do WiFi.mode() abaixo: com o radio em WIFI_MODE_NULL o
  // core le o MAC direto do efuse, em vez de perguntar ao driver.
  Serial.print("MAC do ESP32: ");
  Serial.println(WiFi.macAddress());
  Serial.println();

  // Nao gravar as credenciais na NVS. O padrao do core e persistent(true),
  // o que faz cada WiFi.begin() ter a flash como destino de escrita. Aqui
  // isso nao serve para nada: SSID e senha ja vem compilados no firmware,
  // via secrets.h. Desligar elimina desgaste de flash no cenario em que o
  // roteador fica fora do ar e o dispositivo reinicia a cada ~40s
  // indefinidamente (ver garantirWiFi). De quebra, deixa de existir uma
  // segunda copia da senha fora do binario.
  //
  // A ORDEM IMPORTA e nao e obvia: persistent() apenas grava uma flag
  // interna (_persistent). Quem age sobre ela e wifiLowLevelInit(), que
  // so chama esp_wifi_set_storage(WIFI_STORAGE_RAM) se a flag estiver
  // false - e roda UMA UNICA VEZ, protegida por lowLevelInitDone.
  // Como WiFi.mode() dispara esse init, chamar persistent(false) depois
  // do mode() nao tem efeito nenhum: o storage ja ficou em NVS e nao ha
  // segunda chance. Por isso esta linha vem antes do mode() abaixo.
  WiFi.persistent(false);

  WiFi.mode(WIFI_STA);   // explicito: nunca subir como access point
  WiFi.setAutoReconnect(true);

  // A rota vem antes do garantirWiFi() porque e ele que chama
  // server.begin() no fim. Registrando depois, o servidor passaria alguns
  // milissegundos escutando sem rota nenhuma.
  server.on("/", paginaStatus);

  // O boot e registrado ANTES do garantirWiFi(), e nao depois, para o
  // historico sair na ordem em que as coisas de fato aconteceram. Com o
  // registro depois, a pagina de um aparelho recem-ligado mostrava o boot
  // como evento mais recente, acima da conexao de Wi-Fi que na verdade
  // veio dele - a lista e exibida do mais novo para o mais antigo, entao
  // a ordem de gravacao e a ordem lida de baixo para cima.
  registrar("Boot do ESP32");

  garantirWiFi();

  estadoDesde       = millis();
  ultimaVerificacao = millis();

  // O endereco da pagina so e impresso aqui, depois da rede existir, e
  // sai de WiFi.localIP() e nao da constante. Antes esta linha ficava la
  // em cima, no bloco anterior a conexao, imprimindo SECRET_ESP32_IP:
  // era verdade com o IP fixo ativo e mentira com ele comentado, porque
  // anunciava um endereco em que a pagina nao responderia. localIP() vale
  // nos dois modos.
  Serial.print("Pagina de status: http://");
  Serial.println(WiFi.localIP());

  // Segunda chamada de proposito - garantirWiFi() ja subiu o servidor no
  // fim, e no boot ele sempre passa por aquele caminho. Nao custa nada:
  // WebServer::begin() comeca fechando o que estiver aberto, entao o
  // efeito e fechar e reabrir o mesmo socket de escuta, sem vazar
  // descritor. Fica como rede de seguranca: se algum dia o garantirWiFi()
  // mudar e deixar de subir o servidor, a pagina continua respondendo, e
  // a falha nao aparece meses depois no dia em que alguem precisar dela.
  server.begin();
}

void loop() {
  // Reinicio periodico de higiene. E defesa cega, nao diagnostico: serve
  // contra degradacao que este codigo nao tem como perceber sozinho.
  //
  // Dois casos motivam. O primeiro e fragmentacao de heap - o total livre
  // pode continuar alto enquanto nao existe mais nenhum bloco contiguo
  // grande, e nao ha como checar isso de dentro com confianca. O segundo,
  // mais grave, e a pilha de Wi-Fi entrar num estado em que reporta
  // WL_CONNECTED sem trafego real passar: o ping falha honestamente, a
  // sentinela conclui "alvo desligado" e passa a mandar Wake-on-LAN
  // para sempre num servidor que esta ligado o tempo todo.
  //
  // Nao ha condicao de estado aqui de proposito - reinicia mesmo com o
  // alvo offline no meio das tentativas. Depois do boot o primeiro
  // ciclo pinga em ~30 s, atraso irrelevante contra o intervalo de
  // retentativa de 5 min. Condicionar so criaria um caminho em que o
  // aparelho degradado nunca se recupera justamente por estar ocupado.
  if (millis() > REINICIO_PERIODICO_MS) {
    Serial.println("Reinicio periodico de higiene (24h de funcionamento).");
    reiniciar("reinicio periodico de higiene (24h)");
  }

  garantirWiFi();

  // O heap livre entra no log de cada ciclo como termometro de
  // fragmentacao, nao de vazamento.
  //
  // Criar e destruir uma sessao de ping a cada 5 min, para sempre (~105
  // mil por ano), ja levantou a duvida de vazamento. Ela esta encerrada.
  // O fonte e publico, em components/lwip/apps/ping/ping_sock.c no
  // repositorio espressif/esp-idf, e mostra que esp_ping_delete_session()
  // apenas marca a sessao para encerrar. Quem libera e a task interna do
  // ping, que devolve memoria, buffer do pacote, socket e a si mesma em
  // ate ~1 s. Contra 5 min ate a proxima verificacao, nao vaza.
  Serial.print("Verificando o ");
  Serial.print(ALVO_NOME);
  Serial.print(" (heap livre: ");
  Serial.print(ESP.getFreeHeap());
  Serial.print(" bytes)... ");

  ultimaVerificacao = millis();

  if (alvoResponde()) {
    if (estado != ONLINE) {
      Serial.println("ONLINE.");
      if (estado == OFFLINE) {
        Serial.print("Subiu depois de ");
        Serial.print(tentativasWol);
        Serial.println(" tentativa(s) de Wake-on-LAN.");
        registrar("%s ONLINE apos %d tentativa(s) de WoL", ALVO_NOME, tentativasWol);
      } else {
        registrar("%s ONLINE (ja estava ligado no boot)", ALVO_NOME);
      }
      estado = ONLINE;
      estadoDesde = millis();
      tentativasWol = 0;
    } else {
      // Nao registra: o tique de rotina esgotaria o historico em 100 min.
      Serial.println("continua online.");
    }

    proximaEspera = INTERVALO_MONITORAMENTO_MS;
    esperar(INTERVALO_MONITORAMENTO_MS);
    return;
  }

  // Nao respondeu: tratar como desligado e acordar.
  Serial.println("SEM RESPOSTA.");
  if (estado != OFFLINE) {
    registrar("%s parou de responder", ALVO_NOME);
    estadoDesde = millis();
  }
  estado = OFFLINE;
  tentativasWol++;
  totalWolDesdeBoot++;

  Serial.print("Enviando Wake-on-LAN (tentativa ");
  Serial.print(tentativasWol);
  Serial.print(") para ");
  imprimirMacAlvo();
  Serial.println();

  const int enviados = acordarAlvo();
  if (enviados == 0) {
    Serial.println("  [erro] nenhum magic packet saiu. Problema de rede no ESP32.");
    registrar("ERRO: nenhum magic packet saiu (tentativa %d)", tentativasWol);
  } else {
    registrar("WoL enviado - tentativa %d, %d/%d pacotes", tentativasWol, enviados, REPETICOES);
    Serial.print("  ");
    Serial.print(enviados);
    Serial.print("/");
    Serial.print(REPETICOES);
    Serial.println(" pacotes enviados.");
  }

  // Nas primeiras tentativas espera so o tempo de boot; depois disso
  // espaca, presumindo que o alvo esta fora da tomada. Nunca desiste.
  const unsigned long espera = (tentativasWol <= TENTATIVAS_RAPIDAS)
                                   ? ESPERA_POS_WOL_MS
                                   : INTERVALO_RETENTATIVA_MS;

  proximaEspera = espera;

  Serial.print("Aguardando ");
  Serial.print(espera / 1000UL);
  Serial.println("s antes de verificar de novo.");

  esperar(espera);
  // O proximo ciclo do loop faz o ping de verificacao.
}
