// Template publicavel de configuracao local.
// Copie este arquivo para src/secrets.h e preencha com os valores reais.
// O src/secrets.h esta no .gitignore e NUNCA deve ser commitado.
//
//   cp src/secrets.example.h src/secrets.h
//   $EDITOR src/secrets.h

#pragma once

// --- Wi-Fi ---
// Os valores abaixo sao propositalmente os placeholders que o
// static_assert do main.cpp rejeita. Enquanto estiverem aqui, o build
// falha com uma mensagem clara em vez de gerar um firmware que nao
// conecta. Antes ficavam como string vazia, e a compilacao passava.

// SSID da rede Wi-Fi 2.4 GHz (o ESP32 nao fala 5 GHz).
#define SECRET_WIFI_SSID     "PREENCHER_SSID_AQUI"

// Senha da rede Wi-Fi.
#define SECRET_WIFI_PASSWORD "PREENCHER_SENHA_AQUI"

// --- Alvo ---
// Nome da maquina vigiada, usado nas mensagens da serial e na pagina de
// status. E texto, entao tem trava de compilacao como os campos de Wi-Fi.
#define SECRET_ALVO_NOME     "PREENCHER_NOME_DO_ALVO"

// Os dois campos abaixo tambem tem trava: o build confere a quantidade
// de bytes e rejeita os valores de exemplo. Um MAC com cinco bytes ou um
// IP esquecido no valor do template nao chegam a virar firmware.

// MAC da interface CABEADA do alvo (nao e o MAC do ESP32).
// Linux: ip link   |   Windows: ipconfig /all
#define SECRET_ALVO_MAC      0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF

// IP fixo do alvo, alvo do ping. Precisa ser fixo: com DHCP o endereco
// muda e a sentinela passa a pingar outra maquina, ou nenhuma.
#define SECRET_ALVO_IP       192, 168, 1, 100

// --- Endereco do proprio ESP32 ---

// IP que o aparelho assume. Precisa estar FORA da faixa que o roteador
// distribui por DHCP - dentro dela, o roteador pode entregar o mesmo
// endereco a outro aparelho e os dois somem da rede de forma
// intermitente. Tem trava de quantidade e de valor de exemplo.
#define SECRET_ESP32_IP      192, 168, 1, 197

// Endereco do roteador. Serve de gateway e de DNS.
// Descobrir com: ip route | grep default
#define SECRET_GATEWAY_IP    192, 168, 1, 1

// Mascara da rede local. E o UNICO campo sem trava de valor: 255.255.255.0
// e a mascara legitima da maioria das redes domesticas, entao rejeitar
// esse valor daria falso positivo em quase toda instalacao real. So a
// quantidade de octetos e conferida.
#define SECRET_MASCARA_REDE  255, 255, 255, 0
