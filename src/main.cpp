/*
  ESP32 - Sentinela de Wake-on-LAN do Melchior
  --------------------------------------------
  Compilado e gravado via PlatformIO no VS Code / VSCodium.

  Funcao do dispositivo: manter o melchior ligado.

  Fluxo continuo:
    ONLINE   -> pinga o melchior a cada 5 min. Enquanto responder, so observa.
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
#include "secrets.h"   // credenciais reais - fica fora do git

// API de ping nativa do ESP-IDF. Ja vem no liblwip.a do core,
// nao precisa de biblioteca externa em lib_deps.
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"

// ================= CONFIGURACAO (edite so aqui) =================

// --- Wi-Fi ---
// Credenciais vem de src/secrets.h (arquivo no .gitignore).
// Copie src/secrets.example.h se ainda nao existir.
// A rede precisa ser 2.4 GHz - o ESP32 nao fala 5 GHz.
const char* WIFI_SSID     = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;

// --- Alvo ---
// MAC da placa de rede do Melchior (NAO e o MAC do ESP32).
// Windows: ipconfig /all   |   Linux: ip link
// MAC de exemplo do template (nunca foi o do melchior, esse era o bug):
// byte TARGET_MAC[6] = { 0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E };
byte TARGET_MAC[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };

// IP fixo do melchior (definido por netplan, sem reserva de DHCP).
// E o alvo do ping. Se o IP mudar, mudar aqui tambem.
IPAddress MELCHIOR_IP(192, 168, 1, 100);

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
// martelar a rede quando o melchior esta fisicamente desligado da tomada,
// sem nunca desistir (o requisito e insistir ate ligar).
const int TENTATIVAS_RAPIDAS = 3;

// --- Ping ---
const uint32_t PINGS_POR_CHECAGEM = 3;      // considera online com 1 resposta
const uint32_t PING_TIMEOUT_MS    = 1000;
const uint32_t PING_INTERVALO_MS  = 500;

// ================================================================

// Trava de compilacao: impede gravar um firmware com os placeholders
// do secrets.example.h ainda por preencher. Sem isso o erro so
// apareceria como uma falha de conexao silenciosa na serial.
static bool constexpr mesmaString(const char* a, const char* b) {
  return *a == *b && (*a == '\0' || mesmaString(a + 1, b + 1));
}
static_assert(!mesmaString(SECRET_WIFI_SSID, "PREENCHER_SSID_AQUI"),
              "Preencha SECRET_WIFI_SSID em src/secrets.h antes de compilar.");
static_assert(!mesmaString(SECRET_WIFI_PASSWORD, "PREENCHER_SENHA_AQUI"),
              "Preencha SECRET_WIFI_PASSWORD em src/secrets.h antes de compilar.");

WiFiUDP udp;

enum EstadoMelchior { DESCONHECIDO, ONLINE, OFFLINE };
EstadoMelchior estado = DESCONHECIDO;
int tentativasWol = 0;

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
    memcpy(&pacote[6 + i * 6], TARGET_MAC, 6);
  }

  if (udp.beginPacket(BROADCAST_IP, WOL_PORT) != 1) {
    return false;
  }
  udp.write(pacote, sizeof(pacote));
  return udp.endPacket() == 1;
}

// Dispara o magic packet REPETICOES vezes. Retorna quantas sairam.
int acordarMelchior() {
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

// true = o melchior respondeu pelo menos um ICMP echo.
// Uma unica perda de pacote nao derruba o diagnostico: sao
// PINGS_POR_CHECAGEM tentativas e basta uma resposta.
bool melchiorResponde() {
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();

  ip_addr_t alvo;
  memset(&alvo, 0, sizeof(alvo));
  alvo.type = IPADDR_TYPE_V4;
  // IPAddress e ip4_addr_t guardam os octetos na mesma ordem,
  // entao o cast direto e valido nas duas pontas.
  alvo.u_addr.ip4.addr = (uint32_t)MELCHIOR_IP;

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
    return false;
  }

  esp_ping_start(hdl);

  // Teto de seguranca: se o callback nunca vier, nao trava o loop.
  const unsigned long teto =
      PINGS_POR_CHECAGEM * (PING_TIMEOUT_MS + PING_INTERVALO_MS) + 2000UL;
  const unsigned long inicio = millis();
  while (!pingFinalizado && (millis() - inicio) < teto) {
    delay(50);
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
// firmware acharia que o melchior caiu, mandando WoL a toa.
void garantirWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("Wi-Fi desconectado. Reconectando...");

  // Fecha o socket UDP antes de reconectar. O WiFiUDP reaproveita o
  // mesmo socket para sempre depois de criado (beginPacket retorna cedo
  // se udp_server != -1), entao sem este stop() o magic packet
  // continuaria saindo por um socket criado na sessao de rede anterior.
  udp.stop();

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - inicio > WIFI_TIMEOUT_MS) {
      Serial.println();
      Serial.println("FALHA: nao conectou no Wi-Fi dentro do timeout.");
      Serial.println("Confira SSID/senha em src/secrets.h.");
      Serial.println("Confira tambem se a rede e 2.4 GHz (o ESP32 nao fala 5 GHz).");
      Serial.println("Reiniciando em 10s para tentar de novo...");
      delay(10000);
      ESP.restart();   // dispositivo sem operador: tem que se recuperar sozinho
    }
    delay(300);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("Wi-Fi OK. IP do ESP32: ");
  Serial.println(WiFi.localIP());
}

// Espera em blocos de 1s em vez de um delay() unico de 5 minutos.
// A reconexao do Wi-Fi durante a espera fica por conta do
// setAutoReconnect(true); quem confere de fato e o garantirWiFi()
// no inicio de cada ciclo do loop.
// A subtracao de unsigned long trata o overflow de millis()
// (~49 dias) corretamente - por isso nao se compara millis() > alvo.
void esperar(unsigned long ms) {
  const unsigned long inicio = millis();
  while (millis() - inicio < ms) {
    delay(1000);
  }
}

void imprimirMacAlvo() {
  for (int i = 0; i < 6; i++) {
    if (TARGET_MAC[i] < 0x10) Serial.print("0");
    Serial.print(TARGET_MAC[i], HEX);
    if (i < 5) Serial.print(":");
  }
}

// ---------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("=== Sentinela de Wake-on-LAN do Melchior ===");
  Serial.print("Alvo do ping: ");
  Serial.println(MELCHIOR_IP);
  Serial.print("MAC para o WoL: ");
  imprimirMacAlvo();
  Serial.println();
  Serial.print("Intervalo de monitoramento: ");
  Serial.print(INTERVALO_MONITORAMENTO_MS / 60000UL);
  Serial.println(" min");
  Serial.println();

  WiFi.mode(WIFI_STA);   // explicito: nunca subir como access point

  // Nao gravar as credenciais na NVS. O padrao do core e persistent(true),
  // o que faz cada WiFi.begin() ter a flash como destino de escrita. Aqui
  // isso nao serve para nada: SSID e senha ja vem compilados no firmware,
  // via secrets.h. Desligar elimina desgaste de flash no cenario em que o
  // roteador fica fora do ar e o dispositivo reinicia a cada ~40s
  // indefinidamente (ver garantirWiFi). De quebra, deixa de existir uma
  // segunda copia da senha fora do binario.
  WiFi.persistent(false);

  WiFi.setAutoReconnect(true);
  garantirWiFi();
}

void loop() {
  garantirWiFi();

  Serial.print("Verificando o melchior... ");

  if (melchiorResponde()) {
    if (estado != ONLINE) {
      Serial.println("ONLINE.");
      if (estado == OFFLINE) {
        Serial.print("Subiu depois de ");
        Serial.print(tentativasWol);
        Serial.println(" tentativa(s) de Wake-on-LAN.");
      }
      estado = ONLINE;
      tentativasWol = 0;
    } else {
      Serial.println("continua online.");
    }

    esperar(INTERVALO_MONITORAMENTO_MS);
    return;
  }

  // Nao respondeu: tratar como desligado e acordar.
  Serial.println("SEM RESPOSTA.");
  estado = OFFLINE;
  tentativasWol++;

  Serial.print("Enviando Wake-on-LAN (tentativa ");
  Serial.print(tentativasWol);
  Serial.print(") para ");
  imprimirMacAlvo();
  Serial.println();

  const int enviados = acordarMelchior();
  if (enviados == 0) {
    Serial.println("  [erro] nenhum magic packet saiu. Problema de rede no ESP32.");
  } else {
    Serial.print("  ");
    Serial.print(enviados);
    Serial.print("/");
    Serial.print(REPETICOES);
    Serial.println(" pacotes enviados.");
  }

  // Nas primeiras tentativas espera so o tempo de boot; depois disso
  // espaca, presumindo que o melchior esta fora da tomada. Nunca desiste.
  const unsigned long espera = (tentativasWol <= TENTATIVAS_RAPIDAS)
                                   ? ESPERA_POS_WOL_MS
                                   : INTERVALO_RETENTATIVA_MS;

  Serial.print("Aguardando ");
  Serial.print(espera / 1000UL);
  Serial.println("s antes de verificar de novo.");

  esperar(espera);
  // O proximo ciclo do loop faz o ping de verificacao.
}
