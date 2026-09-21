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
// A lista de maquinas vigiadas. Uma linha por maquina:
//
//     { nome, Ipv4(...), Mac(...), tempo_de_boot_em_segundos }
//
// - nome: aparece na serial e na pagina. Ate 18 caracteres.
// - Ipv4: IP fixo da maquina. Precisa ser fixo - com DHCP o endereco
//   muda e a sentinela passa a pingar outra maquina, ou nenhuma.
// - Mac: da interface CABEADA do alvo, nao do ESP32. E o endereco para
//   onde o magic packet vai.
//   Windows: ipconfig /all   |   Linux: ip link
// - tempo de boot: quanto a maquina leva para responder ao ping depois
//   de acordar, em segundos. Entre 10 e 600.
//
// As barras invertidas no fim de cada linha sao obrigatorias: isto e uma
// macro, e sem elas a definicao termina na primeira quebra de linha.
//
// NESTA VERSAO A LISTA ACEITA UMA MAQUINA SO. Varias entram numa etapa
// futura; por enquanto o build para se houver mais de uma, para que
// nenhuma seja ignorada em silencio.
#define SECRET_ALVOS { \
  { "PREENCHER_NOME_DO_ALVO", Ipv4(192, 168, 1, 100), Mac(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF), 90 }, \
}

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
